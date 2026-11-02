/* spin: anunta ca a pornit, apoi se invarte in user mode pana il opreste un semnal (Ctrl-C in test-ysh) */
#include <stdio.h>

int main(void){
    puts("spin: pornit");
    fflush(stdout);
    for(volatile unsigned long i = 0; ; i++){
    }
}
