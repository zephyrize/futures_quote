#include <futures_quote/client.hpp>
#include <iostream>
static void on_event(void *, const fq_event *e) {
    std::cout << "type=" << e->type << " feed=" << e->feed_id << '\n';
}
int main(int argc, char **argv) {
    try {
        if (argc != 2 && argc != 3)
            return 1;
        fq::Client client(on_event);
        client.open(argv[1]);
        if (argc == 3)
            client.replay(argv[2]);
        std::cout << "version=" << fq_version() << " capabilities=" << client.capabilities(1) << '\n';
        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
