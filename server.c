#include "rdma_common.h"

int main(int argc, char *argv[]) {
    struct resources res;
    int opt;
    
    while ((opt = getopt(argc, argv, "p:d:i:g:G:s:h")) != -1) {
        switch (opt) {
            case 'p': config.tcp_port = atoi(optarg); break;
            case 'd': config.dev_name = strdup(optarg); break;
            case 'i': config.ib_port = atoi(optarg); break;
            case 'g': config.gid_idx = atoi(optarg); break;
#ifdef HAVE_HLTHUNK
            case 'G': config.use_gaudi = atoi(optarg); break;
#endif
            case 's': config.buffer_size = strtoull(optarg, NULL, 0); break;
            case 'h': print_usage(argv[0]); return 0;
            default: print_usage(argv[0]); return 1;
        }
    }
    
    resources_init(&res);
    print_config();
    CHECK(resources_create(&res));
    CHECK(connect_qp(&res));
    CHECK(post_send(&res, IBV_WR_SEND));
    CHECK(poll_completion(&res));
    
    INFO("Message sent successfully\n");
    if (res.buf_type == BUFFER_TYPE_GAUDI) {
        INFO("Used Intel Gaudi optimized buffer\n");
    } else if (res.buf_type == BUFFER_TYPE_DMA_HEAP) {
        INFO("Used DMA-BUF buffer\n");
    }
    
    CHECK(resources_destroy(&res));
    return 0;
}