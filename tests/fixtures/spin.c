/* Debuggee for the end-to-end tests: loops forever calling tick(). */

#include <unistd.h>

volatile unsigned long counter;
volatile char banner[16] = "cogdb-fixture";

__attribute__((noinline)) void tick(void)
{
    counter++;
}

int main(void)
{
    for (;;) {
        tick();
        usleep(1000);
    }
}
