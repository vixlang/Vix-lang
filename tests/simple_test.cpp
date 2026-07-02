#include <cstdio>
int main() {
    fprintf(stderr, "TEST STDOUT MESSAGE\n");
    fprintf(stdout, "TEST STDOUT MESSAGE\n");
    printf("HELLO FROM TEST\n");
    fflush(stdout);
    fflush(stderr);
    return 42;
}
