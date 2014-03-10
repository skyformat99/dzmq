#include <sys/syscall.h>
#include <ctype.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include "dzutil.h"
#include "dzlog.h"

/**
 * @brief is_dir Check a path is a directory.
 *
 * @param path The path input.
 *
 * @return 1 for yes and -1 for no.
 */
int is_dir(const char *path)
{
    struct stat st;
    if(stat(path, &st)<0)
    {
        LOG_PRINT(LOG_WARNING, "Path[%s] is Not Existed!", path);
        return -1;
    }
    if(S_ISDIR(st.st_mode))
    {
        LOG_PRINT(LOG_INFO, "Path[%s] is A Dir.", path);
        return 1;
    }
    else
        return -1;
}

/**
 * @brief mk_dir It create a new directory with the path input.
 *
 * @param path The path you want to create.
 *
 * @return  1 for success and -1 for fail.
 */
int mk_dir(const char *path)
{
    if(access(path, 0) == -1)
    {
        LOG_PRINT(LOG_INFO, "Begin to mk_dir()...");
        int status = mkdir(path, S_IRWXU | S_IRWXG | S_IROTH | S_IXOTH);
        if(status == -1)
        {
            LOG_PRINT(LOG_WARNING, "mkdir[%s] Failed!", path);
            return -1;
        }
        LOG_PRINT(LOG_INFO, "mkdir[%s] sucessfully!", path);
        return 1;
    }
    else
    {
        LOG_PRINT(LOG_WARNING, "Path[%s] is Existed!", path);
        return -1;
    }
}

/**
 * @brief mk_dirs It creates a multi-level directory like ./img/330/28/557.
 *
 * @param dir The path of a multi-level directory.
 *
 * @return  1 for success and -1 for fail.
 */
int mk_dirs(const char *dir)
{
    char tmp[512];
    char *p;
    if (strlen(dir) == 0 || dir == NULL)
    {
        LOG_PRINT(LOG_WARNING, "strlen(dir) is 0 or dir is NULL.");
        return -1;
    }
    memset(tmp, 0, sizeof(tmp));
    strncpy(tmp, dir, strlen(dir));
    if (tmp[0] == '/' && tmp[1]== '/')
        p = strchr(tmp + 2, '/');
    else
        p = strchr(tmp, '/');
    if (p)
    {
        *p = '\0';
        mkdir(tmp,0755);
        chdir(tmp);
    }
    else
    {
        mkdir(tmp,0755);
        chdir(tmp);
        return 1;
    }
    mk_dirs(p + 1);
    return 1;
}

static char *
get_zframe_str(zframe_t *self, const char *prefix) {
    assert (self);
    char *msg_data = (char *)malloc(256 * sizeof(char));
    if (prefix)
        strcat(msg_data, prefix);
    byte *data = zframe_data (self);
    size_t size = zframe_size (self);

    int is_bin = 0;
    uint char_nbr;
    for (char_nbr = 0; char_nbr < size; char_nbr++)
        if (data [char_nbr] < 9 || data [char_nbr] > 127)
            is_bin = 1;

    int len = snprintf(NULL, 0, "[%03d]", (int)size);
    char *temp = (char *)malloc((len + 1) * sizeof(char));
    snprintf(temp, len + 1, "[%03d]", (int)size);
    strcat(msg_data, temp);
    free(temp);

    size_t max_size = is_bin? 35: 70;
    const char *ellipsis = "";
    if (size > max_size) {
        size = max_size;
        ellipsis = "...";
    }
    for (char_nbr = 0; char_nbr < size; char_nbr++) {
        if (is_bin) {
            int len = snprintf(NULL, 0, "%02X", (unsigned char) data [char_nbr]);
            char *temp = (char *)malloc((len + 1) * sizeof(char));
            snprintf(temp, len + 1, "%02X", (unsigned char) data [char_nbr]);
            strcat(msg_data, temp);
            free(temp);
        } else {
            int len = snprintf(NULL, 0, "%c", data[char_nbr]);
            char *temp = (char *)malloc((len + 1) * sizeof(char));
            snprintf(temp, len + 1, "%c", data[char_nbr]);
            strcat(msg_data, temp);
            free(temp);
        }
    }
    strcat(msg_data, ellipsis);
    return msg_data;
}

void zmsg_log_dump(zmsg_t *msg, const char *prefix) {
    if (!msg) {
        LOG_PRINT(LOG_DEBUG, "%s:%s", prefix, "NULL");
        return;
    }
    zframe_t *frame = zmsg_first(msg);
    int frame_nbr = 0;
    char msg_data[256] = "";
    while (frame && frame_nbr++ < 10) {
        char temp[10] = "";
        sprintf(temp, " %s-%d:", "frame", frame_nbr);
        strcat(msg_data, temp);
        char *frame_str = get_zframe_str(frame, NULL);
        strcat(msg_data, frame_str);
        frame = zmsg_next(msg);
        free(frame_str);
    }
    LOG_PRINT(LOG_DEBUG, "%s:%s", prefix, msg_data);

    /**
    zmsg_t *debug_msg = zmsg_dup(msg);
    int msglen = zmsg_size(debug_msg);
    char msg_data[256];
    for (int i = 0; i < msglen; i++){
        char temp[10];
        sprintf(temp, " %s-%d:", "frame", i);
        strcat(msg_data, temp);
        strcat(msg_data, zmsg_popstr(debug_msg));
    }
    LOG_PRINT(LOG_DEBUG, "%s:%s", prefix, msg_data);
    zmsg_destroy(&debug_msg);
    */
}
