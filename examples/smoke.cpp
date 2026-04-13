#include <spp/base.h>

int main() {
    const char* text = "spp";
    return spp::Libc::strlen(text) == 3 ? 0 : 1;
}

