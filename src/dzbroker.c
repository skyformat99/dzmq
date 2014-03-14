
#include "dzbroker.h"
#include "dzservice.h"
#include "mdp_client.h"
#include "mdp_worker.h"
#include "dzlog.h"
#include "dzcommon.h"
#include "dzutil.h"

struct _dz_broker {
    zctx_t *ctx;            // Context
    const char *name;       // Broker name
    char **remote;          // Remote brokers' name
    int rlen;               // number of remote brokers
    void *localfe;
    void *localbe;
    void *cloudfe;
    void *cloudbe;
    void *statefe;
    void *statebe;
    int local_capacity;
    int cloud_capacity;
    zlist_t *workers;
    zhash_t *services;      // Hash of known services
    zhash_t *workers_hash;  // Hash of known workers
    zlist_t *waiting;       // List of waiting workers
    uint64_t heartbeat_at;  // When to send HEARTBEAT
};

static void
    s_broker_worker_msg (dz_broker *self, zmsg_t *msg, bool from_local);
static void
    s_broker_client_msg (dz_broker *self, zmsg_t *msg, bool from_local);
static void
    s_broker_purge (dz_broker *self);
static void *
    client_task_mdp (void *args);
static void *
    worker_task_mdp(void *args);


dz_broker *
dz_broker_new(const char *local, char **remote, int rlen) {
    dz_broker *self = (dz_broker *) zmalloc(sizeof(dz_broker));
    self->name = local;
    self->remote = (char **)malloc(sizeof(char *) * rlen);
    memcpy(self->remote, remote, rlen * sizeof(char *));
    self->rlen = rlen;
    self->ctx = zctx_new();

    self->localfe = zsocket_new(self->ctx, ZMQ_ROUTER);
    zsocket_bind(self->localfe, "ipc://%s-localfe.ipc", local);

    self->localbe = zsocket_new(self->ctx, ZMQ_ROUTER);
    zsocket_bind(self->localbe, "ipc://%s-localbe.ipc", local);

    self->cloudfe = zsocket_new(self->ctx, ZMQ_ROUTER);
    zsocket_set_identity(self->cloudfe, local);
    zsocket_bind(self->cloudfe, "ipc://%s-cloud.ipc", local);

    self->cloudbe = zsocket_new(self->ctx, ZMQ_ROUTER);
    zsocket_set_identity(self->cloudbe, local);
    for (int i = 0; i < rlen; i++) {
        char *peer = remote[i];
        LOG_PRINT(LOG_DEBUG, "I: connecting to cloud frontend at '%s'\n", peer);
        zsocket_connect(self->cloudbe, "ipc://%s-cloud.ipc", peer);
    }

    self->statebe = zsocket_new(self->ctx, ZMQ_PUB);
    zsocket_bind(self->statebe, "ipc://%s-state.ipc", local);

    self->statefe = zsocket_new(self->ctx, ZMQ_SUB);
    zsocket_set_subscribe(self->statefe, "");
    for (int i = 0; i < rlen; i++) {
        char *peer = remote[i];
        LOG_PRINT(LOG_DEBUG, "I: connecting to state backend at '%s'\n", peer);
        zsocket_connect(self->statefe, "ipc://%s-state.ipc", peer);
    }

    self->local_capacity = 0;
    self->cloud_capacity = 0;
    self->workers = zlist_new();

    self->services = zhash_new();
    self->workers_hash = zhash_new();
    self->waiting = zlist_new();
    self->heartbeat_at = zclock_time() + HEARTBEAT_INTERVAL;

    return self;
}

void
dz_broker_destory(dz_broker **self_p) {
    assert(self_p);
    if (*self_p) {
        dz_broker *self = *self_p;
        while (zlist_size(self->workers)) {
            zframe_t *frame = (zframe_t *)zlist_pop(self->workers);
            zframe_destroy(&frame);
        }
        zlist_destroy(&self->workers);
        zctx_destroy(&self->ctx);
        zhash_destroy(&self->services);
        zhash_destroy(&self->workers_hash);
        zlist_destroy(&self->waiting);
        free(self->remote);
        free(self);
        *self_p = NULL;
    }
}

void
dz_broker_sim_client(dz_broker *self, int client_num, int verbose) {
    for (int i = 0; i < client_num; i++) {
        const char *addr = self->name;
        bind_info binfo = {i, addr, verbose};
        // TODO: a strange segmentation fault bug
        /*bind_info binfo = {i, self->name, verbose};*/
        zthread_new(client_task_mdp, &binfo);
        /*LOG_PRINT(LOG_DEBUG, "binfo:%d, %s, %d", binfo.nbr_id, binfo.bind_addr, binfo.verbose);*/
    }
}

void
dz_broker_sim_worker(dz_broker *self, int worker_num, int verbose) {
    for (int i = 0; i < worker_num; i++) {
        const char *addr = self->name;
        bind_info binfo = {i, addr, verbose};
        // TODO: a strange segmentation fault bug
        /*bind_info binfo = {i, self->name, verbose};*/
        zthread_new(worker_task_mdp, &binfo);
    }
}

//  Here is the implementation of the methods that work on a worker.
//  Lazy constructor that locates a worker by identity, or creates a new
//  worker if there is no worker already with that identity.

//  Delete worker by dz_broker.
void s_broker_worker_delete (dz_broker *self, worker_t *worker) {
    /*s_worker_delete(worker, disconnect);*/
    self->local_capacity--;
    zlist_remove (self->workers, worker->identity);
    zlist_remove (self->waiting, worker);
    //  This implicitly calls s_worker_destroy
    zhash_delete (self->workers_hash, worker->identity);
    // TODO: notify other brokers about local_capacity change
}

static worker_t *
s_worker_require (dz_broker *self, zframe_t *address)
{
    assert (address);

    //  self->workers is keyed off worker identity
    char *identity = zframe_strhex (address);
    worker_t *worker =
        (worker_t *) zhash_lookup (self->workers_hash, identity);

    if (worker == NULL) {
        worker = (worker_t *) zmalloc (sizeof (worker_t));
        /*worker->broker = self;*/
        worker->identity = identity;
        worker->address = zframe_dup (address);
        zhash_insert (self->workers_hash, identity, worker);
        zhash_freefn (self->workers_hash, identity, s_worker_destroy);
        LOG_PRINT(LOG_DEBUG, "Registering new worker: %s", identity);
    }
    else
        free (identity);
    return worker;
}

void s_broker_worker_msg(dz_broker *self, zmsg_t *msg, bool from_local) {
    // At least: identity, empty, header, command
    // or identity, empty, header, command [peer_name] service data
    assert(zmsg_size(msg) >= 4);
    zframe_t *sender = zmsg_pop (msg);
    zframe_t *empty  = zmsg_pop (msg);
    zframe_t *header = zmsg_pop (msg);
    assert( zframe_streq(header, MDPW_WORKER) == true );

    zframe_t *command = zmsg_pop(msg);

    /*char *identity = zframe_strhex(sender);*/
    /*int worker_ready = (zhash_lookup (self->workers_hash, identity) != NULL);*/
    /*free(identity);*/
    worker_t *worker = s_worker_require(self, sender);

    if (zframe_streq(command, MDPW_READY)) {
        if (from_local) {
            self->local_capacity++;
            zlist_append(self->workers, sender);
        }

        //  Attach worker to service and mark as idle
        zframe_t *service_frame = zmsg_pop (msg);
        zlist_append (self->waiting, worker);
        worker->expiry = zclock_time () + HEARTBEAT_EXPIRY;

        if (worker->service) {
            free(worker->service);
            // worker->service = NULL;
        }
        worker->service = zframe_strdup(service_frame);

        zframe_destroy (&service_frame);
        char *identity = zframe_strhex(sender);
        LOG_PRINT(LOG_INFO, "worker-%s ready and broker worker_t created", identity);
        free(identity);
        zmsg_destroy(&msg);
    }  else if (zframe_streq(command, MDPW_REPORT_LOCAL)) {
        // handle self client's request, and send back to local client
        zmsg_log_dump(msg, "REPORT TO LOCAL");
        zframe_t *client = zmsg_unwrap (msg);
        /*zmsg_pushstr (msg, worker->service);*/
        zmsg_pushstr (msg, MDPC_REPORT);
        zmsg_pushstr (msg, MDPC_CLIENT);
        zmsg_wrap (msg, client);

        if (from_local) {
            self->local_capacity++;
            zlist_append(self->workers, sender);
        }

        //  Route reply to client if we still need to
        if (msg) {
            zmsg_send (&msg, self->localfe);
        }
    } else if (zframe_streq(command, MDPW_REPORT_CLOUD)) {
        if (from_local) {
            // handle peer cient's request, and send back to cloud
            self->local_capacity++;
            zlist_append(self->workers, sender);

            zframe_t *client = zmsg_unwrap (msg);
            zframe_t *peer_name = zmsg_pop(msg);

            char *data = (char *) zframe_data (peer_name);
            //  Route reply to cloud if it's addressed to a broker
            for (int i = 0; msg && i < self->rlen; i++) {
                size_t size = zframe_size (peer_name);
                if (size == strlen(self->remote[i]) &&  memcmp (data, self->remote[i], size) == 0) {
                    zmsg_pushstr (msg, MDPW_REPORT_CLOUD);
                    zmsg_pushstr (msg, MDPW_WORKER);
                    zmsg_wrap (msg, client);
                    zmsg_push(msg, peer_name);

                    zmsg_send (&msg, self->cloudfe);
                }
            }

        }
        else {
            // msg: service data

            zmsg_pushstr (msg, MDPC_REPORT);
            zmsg_pushstr (msg, MDPC_CLIENT);
            zmsg_wrap (msg, sender);

            zmsg_log_dump(msg, "WORK DONE BY PEER");
            zmsg_send (&msg, self->localfe);
        }

    } else if (zframe_streq(command, MDPW_HEARTBEAT)) {
        worker->expiry = zclock_time() + HEARTBEAT_EXPIRY;
    } else if (zframe_streq(command, MDPW_DISCONNECT)) {
        s_broker_worker_delete(self, worker);
    } else {
        LOG_PRINT(LOG_ERROR, "invalid worker message");
    }
    zframe_destroy(&command);
    zmsg_destroy(&msg);
}

static void
s_broker_client_msg(dz_broker *self, zmsg_t *msg, bool from_local) {
    // At least: identity, empty, header, service_name, body
    assert (zmsg_size (msg) >= 5);

    zframe_t *sender = zmsg_pop (msg);

    // TODO: check whether service available
    if (self->local_capacity) {
        if (from_local) {
            zframe_t *empty  = zmsg_pop (msg);
            zframe_t *header = zmsg_pop (msg);
            zframe_destroy(&empty);
            zframe_destroy(&header);
            zframe_t *service = zmsg_pop (msg);

            zmsg_wrap(msg, sender);
            zmsg_push(msg, service);
            zmsg_pushstr(msg, MDPW_REQUEST);
        } else {
            // send other peer's client request
            zframe_t *header = zmsg_pop(msg);
            assert(zframe_streq(header, MDPC_CLIENT));
            zframe_destroy (&header);

            zframe_t *command = zmsg_pop(msg);
            assert(zframe_streq(command, MDPC_REPOST));
            zframe_destroy (&command);

            zmsg_pushstr(msg, MDPW_REPOST);
        }
        zmsg_pushstr(msg, MDPW_WORKER);
        zframe_t *frame = (zframe_t *)zlist_pop(self->workers);
        zmsg_wrap (msg, frame);

        zmsg_send (&msg, self->localbe);
        self->local_capacity--;

    } else {
        zframe_t *empty  = zmsg_pop (msg);
        zframe_t *header = zmsg_pop (msg);
        zframe_t *service = zmsg_pop (msg);
        zframe_destroy(&empty);
        zframe_destroy(&header);

        //  Route to random broker peer
        int peer = randof(self->rlen);

        zmsg_wrap(msg, sender);
        zmsg_pushstr(msg, self->name);
        /*zmsg_pushmem (msg, self->remote[peer], strlen(self->remote[peer]));*/
        /*zmsg_pushmem (msg, self->name, strlen(self->name));*/
        zmsg_push(msg, service);
        zmsg_pushstr(msg, MDPC_REPOST);
        zmsg_pushstr(msg, MDPC_CLIENT);

        zmsg_pushmem (msg, self->remote[peer], strlen(self->remote[peer]));
        zmsg_log_dump(msg, "Route msg");

        zmsg_send (&msg, self->cloudbe);
        LOG_PRINT(LOG_DEBUG, "Route to random broker peer %s", self->remote[peer]);
    }

    /**
    //  Send a NAK message back to the client.
    zmsg_push (msg, zframe_dup (service_frame));
    zmsg_pushstr (msg, MDPC_NAK);
    zmsg_pushstr (msg, MDPC_CLIENT);
    zmsg_wrap (msg, zframe_dup (sender));
    zmsg_send (&msg, self->socket);
    */
}

void dz_broker_main_loop_mdp(dz_broker *self) {
    while (true) {
        zmq_pollitem_t primary [] = {
            { self->localbe, 0, ZMQ_POLLIN, 0 },
            { self->cloudbe, 0, ZMQ_POLLIN, 0 },
            { self->statefe, 0, ZMQ_POLLIN, 0 }
        };
        //  If we have no workers ready, wait indefinitely
        int rc = zmq_poll (primary, 3,
            self->local_capacity? 1000 * ZMQ_POLL_MSEC: -1);
        if (rc == -1)
            break;              //  Interrupted

        //  Track if capacity changes during this iteration
        int previous = self->local_capacity;
        zmsg_t *msg = NULL;     //  Reply from local worker

        if (primary [0].revents & ZMQ_POLLIN) {
            msg = zmsg_recv(self->localbe);
            if (!msg)
                break;          //  Interrupted
            s_broker_worker_msg(self, msg, true);
        }
        // Or handle reply from peer broker, in fact this msg only goes to localfe?
        else
        if (primary [1].revents & ZMQ_POLLIN) {
            msg = zmsg_recv(self->cloudbe);
            if (!msg)
                break;          //  Interrupted
            zframe_t *remote_name = zmsg_pop(msg);
            zframe_destroy(&remote_name);
            s_broker_worker_msg(self, msg, false);
        }

        //  If we have input messages on our statefe sockets, we
        //  can process these immediately:

        if (primary[2].revents & ZMQ_POLLIN) {
            char *peer = zstr_recv(self->statefe);
            char *status = zstr_recv(self->statefe);
            self->cloud_capacity = atoi(status);
            free(peer);
            free(status);
        }

        //  Now route as many clients requests as we can handle. If we have
        //  local capacity, we poll both localfe and cloudfe. If we have cloud
        //  capacity only, we poll just localfe. We route any request locally
        //  if we can, else we route to the cloud.

        while (self->local_capacity + self->cloud_capacity) {
            zmq_pollitem_t secondary [] = {
                { self->localfe, 0, ZMQ_POLLIN, 0 },
                { self->cloudfe, 0, ZMQ_POLLIN, 0 }
            };
            if(self->local_capacity)
                rc = zmq_poll(secondary, 2, 0);
            else
                rc = zmq_poll(secondary, 1, 0);
            assert (rc >= 0);

            bool from_local = true;
            if (secondary [0].revents & ZMQ_POLLIN) {
                msg = zmsg_recv(self->localfe);
            }
            else if (secondary [1].revents & ZMQ_POLLIN) {
                msg = zmsg_recv(self->cloudfe);
                from_local = false;
                LOG_PRINT(LOG_DEBUG, "GET msg from broker peer");
            } else {
                break;      //  No work, go back to primary
            }
            s_broker_client_msg(self, msg, from_local);
        }
        //  We broadcast capacity messages to other peers; to reduce chatter,
        //  we do this only if our capacity changed.

        printf("dzbroker-%s local_capacity = %d\n", self->name, self->local_capacity);
        if (self->local_capacity != previous) {
            //  We stick our own identity onto the envelope
            zstr_sendm(self->statebe, self->name);
            //  Broadcast new capacity
            zstr_sendf(self->statebe, "%d", self->local_capacity);
        }
    }
}

static void *
client_task_mdp (void *args)
{
    static int total = 0;
    static int success  = 0;
    static int fail = 0;
    static int lost = 0;
    bind_info *binfo = (bind_info *)args;
    int client_id = binfo->nbr_id;
    const char *bind_addr = binfo->bind_addr;
    int verbose = binfo->verbose;

    zctx_t *ctx = zctx_new();
    char endpoint[256];
    sprintf(endpoint, "ipc://%s-localfe.ipc", bind_addr);
    mdp_client_t *mdp_client = mdp_client_new(endpoint, verbose);

    while (true) {
        sleep (randof (5));
        int burst = randof (15);
        while (burst--) {
            zmsg_t *request = zmsg_new();
            char task_id [5];
            sprintf (task_id, "%04X", randof (0x10000));
            zmsg_pushstr(request, task_id);

            //  Send request with random hex ID
            LOG_PRINT(LOG_INFO, "client-%d new request %s", client_id, task_id);
            mdp_client_send(mdp_client, "echo", &request);
            __sync_fetch_and_add(&total, 1);

            zmsg_t *reply = mdp_client_timeout_recv(mdp_client, NULL, NULL, client_id, task_id);
            if (reply == NULL) {
                __sync_fetch_and_add(&lost, 1);
                printf("total:%d, success:%d, fail:%d\n", total, success, fail);
                break;
            } else {
                zmsg_log_dump(reply, "client recv back:):):):):)");
                zframe_t *data = zmsg_first(reply);
                if (strcmp(task_id, zframe_strdup(data)) == 0) {
                    __sync_fetch_and_add(&success, 1);
                    printf("total:%d, success:%d, fail:%d, lost:%d\n", total, success, fail, lost);
                    zmsg_log_dump(reply, "client recv back success");
                } else {
                    __sync_fetch_and_add(&fail, 1);
                    printf("%s------%s\n", task_id, zframe_strdup(data));
                    printf("total:%d, success:%d, fail:%d, lost:%d\n", total, success, fail, lost);
                    zmsg_log_dump(reply, "client recv back fail");
                }
            }
            zmsg_destroy(&reply);
        }
    }
    zctx_destroy (&ctx);
    return NULL;
}

static void *
worker_task_mdp(void *args) {
    bind_info *binfo = (bind_info *)args;
    int worker_id = binfo->nbr_id;
    const char *bind_addr = binfo->bind_addr;
    int verbose = binfo->verbose;

    zctx_t *ctx = zctx_new ();
    char endpoint[256];
    sprintf(endpoint, "ipc://%s-localbe.ipc", bind_addr);
    mdp_worker_t *mdp_worker = mdp_worker_new(endpoint, "echo", verbose);

    //  Process messages as they arrive
    while (true) {
        zframe_t *reply_to;
        zmsg_t *request = mdp_worker_recv (mdp_worker, &reply_to);
        if (request == NULL)
            break;              //  Worker was interrupted
        zmsg_log_dump(request, "WORKER RECV");

        //  Workers are busy for 0/1 seconds
        sleep (randof (2));
        //  Echo message
        mdp_worker_send (mdp_worker, &request, reply_to);
        LOG_PRINT(LOG_INFO, "worker-%d done and send back", worker_id);
        zframe_destroy (&reply_to);
    }
    mdp_worker_destroy(&mdp_worker);
    zctx_destroy (&ctx);
    return NULL;
}

