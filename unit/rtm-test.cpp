#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <immintrin.h>

int main(int argc, char *argv[]){
  volatile int result=-1;
  unsigned status;

  while(result != 1){
    if ((status = _xbegin()) == _XBEGIN_STARTED) {
      result=1;
      _xend();
    }else{
      printf("rtmCheck: Transaction failed\n");
      printf("Trying again ...\n");
      sleep(5);
    }
  }

  printf("rtmCheck : Result is %d\n", result);
  return 0;
}