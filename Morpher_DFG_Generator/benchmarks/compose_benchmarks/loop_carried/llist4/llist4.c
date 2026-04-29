#include <stdio.h>
#include <stdlib.h>

#define SIZE 20
#define TARGET 133

typedef struct Node {
    int data;
    struct Node* next;
} Node;

Node* head  = NULL;

__attribute__((noinline))
int llist4(Node* hd, int target) {
    while (hd) {
        #ifdef CGRA_COMPILER
        please_map_me();
        #endif

        if (hd->data == target) return 1;
        hd = hd->next;
        if (!hd) break;

        if (hd->data == target) return 1;
        hd = hd->next;
        if (!hd) break;

        if (hd->data == target) return 1;
        hd = hd->next;
        if (!hd) break;

        if (hd->data == target) return 1;
        hd = hd->next;
    }
    return -1;
}

void create_list() {
    Node* curr = NULL;
    for (int i = SIZE - 1; i >= 0; i--) {
        Node* new_node = (Node*)malloc(sizeof(Node));
        new_node->data = (i * 13) % 256;
        new_node->next = curr;
        curr = new_node;
    }
    head = curr;
}

void free_list(Node* hd) {
    while (hd) {
        Node* temp = hd;
        hd = hd->next;
        free(temp);
    }
}

int main() {
    create_list();

    int result = llist4(head, TARGET);
    // if (result == 1) {
    //     printf("Found %d in the list.\n", TARGET);
    // } else {
    //     printf("%d not found in the list.\n", TARGET);
    // }
    free_list(head);
    return 0;
}