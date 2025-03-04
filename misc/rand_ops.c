#include <stdlib.h>

#include "rand_ops.h"

int rand_get(unsigned int seed, unsigned int num) {
    srand(seed);
    while (num--) {
        rand();
    }
    return rand();
}

long random_get() {

}

int rand_get_0to3600(unsigned int seed, unsigned int num) {
    return (rand_get(seed, num)%3600);
}

