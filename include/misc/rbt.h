#include <misc/containerof.h>
#include <stdint.h>

#define rbt_entry(ptr, type, member) container_of(ptr, type, member)
#define rbt_parent(n) ((n)->parent)
#define RBTREE_COMPARE(a, b) ((a)->key - (b)->key)

enum rbt_node_color { TREE_NODE_RED, TREE_NODE_BLACK };

struct rbt_node {
    int data;
    enum rbt_node_color color;
    struct rbt_node *left;
    struct rbt_node *right;
    struct rbt_node *parent;
};

struct rbt {
    struct rbt_node *root;
};

struct rbt *rbt_create(void);
struct rbt_node *rbt_find_min(struct rbt_node *node);
void rb_delete(struct rbt *tree, struct rbt_node *z);
struct rbt_node *rbt_search(struct rbt_node *root, int data);
void rbt_remove(struct rbt *tree, int data);
void rbt_insert(struct rbt *tree, struct rbt_node *new_node);
