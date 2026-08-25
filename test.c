#include <stdio.h>
#include <stdint.h>

struct CellSpursJob256 {
    uint64_t eaBinary;
    uint32_t sizeBinary;
    uint32_t sizeDmaList;
    uint64_t eaDmaList;
};

int main() {
    printf("eaBinary offset: %lu\n", offsetof(struct CellSpursJob256, eaBinary));
    return 0;
}
