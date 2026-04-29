#include <string.h>
#include <stdio.h>


#define SIZE  21
#define SBOX_SIZE 1024

int A[SIZE];
int SBOX[SBOX_SIZE];
 int f;

__attribute__((noinline))
void blowfish(){
 int left = 1; //0x12345678;
 int right = 2; //0x9abcdef0;
 int temp;
// unsigned int f;

   for (int i=0;i<SIZE; i++){
    #ifdef CGRA_COMPILER
      please_map_me();
      #endif
      f = ((left >> 3) ^ (left << 2)) + SBOX[left & 0xFF]; // B_ENC mock encryption function
      // f = ((left >> 3) ^ (left << 2)) + 123; //debug
      // f = ((left)); //debug
      right = right ^ (f ^ A[i]);

      // right = right + left;
      // left = left - right; 
      // right = right - left; // debug

      temp = right;
      right = left;
      left = temp;

      // printf("%d\n", f);
   }
   // printf("%d\n", f);

}

int main(){

int i,j;

for (i=0;i<SIZE; i++){
    A[i] = i * 2654435761u;
    }
for (i = 0; i < SBOX_SIZE; ++i)
        SBOX[i] = i ^ 0xA5A5A5A5;

blowfish();

printf("%d\n", f);
    

return 0;

}
