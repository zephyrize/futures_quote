#include <futures_quote/fq.h>
#include <stdio.h>
static void on_event(void *user, const fq_event *e) {
    (void)user;
    printf("type=%u feed=%u instrument=%llu flags=%u\n", e->type, e->feed_id,
           (unsigned long long)e->instrument_id, e->flags);
}
int main(int argc, char **argv) {
    fq_client *client = NULL;
    fq_callbacks callbacks = {sizeof(callbacks), FQ_ABI_VERSION, on_event, NULL};
    if ((argc != 2 && argc != 3) || fq_create(&callbacks, &client) != FQ_OK)
        return 1;
    int rc = fq_open(client, argv[1]);
    if (rc == FQ_OK && argc == 3)
        rc = fq_replay_pcap(client, argv[2]);
    if (rc < 0) {
        char message[512];
        fq_last_error(client, message, sizeof(message));
        fprintf(stderr, "%s\n", message);
    }
    /* For live: fq_start(client); ... fq_request_stop(client); fq_join(client). */
    fq_destroy(client);
    return rc < 0 ? 1 : 0;
}
