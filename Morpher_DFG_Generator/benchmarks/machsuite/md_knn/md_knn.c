/* MachSuite: md/knn — molecular dynamics with k-nearest-neighbour list.
   Integer version: avoids loop-carried float phi nodes.
   CGRA kernel: inner j loop over pre-gathered neighbour positions.
   Neighbour positions are pre-gathered into flat int arrays before the
   CGRA kernel so the kernel body has no integer-from-memory loads. */

#include <stdio.h>
#include <stdint.h>

#define nAtoms        64
#define maxNeighbors  64

int32_t position_x[nAtoms];
int32_t position_y[nAtoms];
int32_t position_z[nAtoms];
int32_t force_x[nAtoms];
int32_t force_y[nAtoms];
int32_t force_z[nAtoms];
int32_t NL[nAtoms * maxNeighbors];

/* Pre-gathered neighbour positions — filled before the CGRA kernel call. */
int32_t nbr_x[nAtoms * maxNeighbors];
int32_t nbr_y[nAtoms * maxNeighbors];
int32_t nbr_z[nAtoms * maxNeighbors];

__attribute__((noinline))
void md_knn() {
    int32_t delx, dely, delz;
    int32_t i_x, i_y, i_z, fx, fy, fz;
    int i, j;

    for (i = 0; i < nAtoms; i++) {
        i_x = position_x[i];
        i_y = position_y[i];
        i_z = position_z[i];
        fx = 0; fy = 0; fz = 0;
        for (j = 0; j < maxNeighbors; j++) {
            #ifdef CGRA_COMPILER
            please_map_me();
            #endif
            int k = i*maxNeighbors + j;
            delx = i_x - nbr_x[k];
            dely = i_y - nbr_y[k];
            delz = i_z - nbr_z[k];
            fx += delx;
            fy += dely;
            fz += delz;
        }
        force_x[i] = fx;
        force_y[i] = fy;
        force_z[i] = fz;
    }
}

int main() {
    int i, j;
    for (i = 0; i < nAtoms; i++) {
        position_x[i] = i % 4 + 1;
        position_y[i] = i / 4 + 1;
        position_z[i] = i % 3 + 1;
        force_x[i] = 0; force_y[i] = 0; force_z[i] = 0;
        for (j = 0; j < maxNeighbors; j++) {
            int jidx = (i + j + 1) % nAtoms;
            NL[i*maxNeighbors + j] = jidx;
            /* Pre-gather neighbour positions */
            nbr_x[i*maxNeighbors + j] = position_x[jidx];
            nbr_y[i*maxNeighbors + j] = position_y[jidx];
            nbr_z[i*maxNeighbors + j] = position_z[jidx];
        }
    }
    md_knn();
    for (i = 0; i < 4; i++)
        printf("force_x[%d]=%d\n", i, force_x[i]);
    return 0;
}
