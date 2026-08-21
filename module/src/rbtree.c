#include "rbtree.h"

/* -------------------------------------------------------------------------- */
/*                                  接口定义                                  */
/* -------------------------------------------------------------------------- */

struct rb_node *
rb_get_parent(const struct rb_node *rb)
{
    return (struct rb_node *)(rb->rb_parent_color & ~3);
}

void
rb_set_parent(struct rb_node *rb, struct rb_node *parent)
{
    rb->rb_parent_color = (size_t)parent | (rb->rb_parent_color & 3);
}

enum rb_color
rb_get_color(const struct rb_node *rb)
{
    return (enum rb_color)(rb->rb_parent_color & 1);
}

void
rb_set_color(struct rb_node *rb, const enum rb_color color)
{
    rb->rb_parent_color = (rb->rb_parent_color & ~1) | color;
}

uint8_t
rb_is_red(const struct rb_node *rb)
{
    return rb_get_color(rb) == RB_RED;
}

uint8_t
rb_is_black(const struct rb_node *rb)
{
    return rb_get_color(rb) == RB_BLACK;
}

void
rb_set_red(struct rb_node *rb)
{
    rb_set_color(rb, RB_RED);
}

void
rb_set_black(struct rb_node *rb)
{
    rb_set_color(rb, RB_BLACK);
}

uint8_t
rb_is_unlinked(const struct rb_node *rb)
{
    return rb->rb_parent_color == (size_t)rb;
}

void
rb_link_node(struct rb_node *node, struct rb_node *parent, struct rb_node **rb_link)
{
    node->rb_parent_color = (size_t)parent;
    node->rb_left = node->rb_right = NULL;
    *rb_link                       = node;
}

void
rb_rotate_left(struct rb_node *node, struct rb_root *root)
{
    struct rb_node *right  = node->rb_right;
    struct rb_node *parent = rb_get_parent(node);

    if ((node->rb_right = right->rb_left))
        rb_set_parent(right->rb_left, node);

    right->rb_left = node;
    rb_set_parent(right, parent);

    if (parent) {
        if (node == parent->rb_left)
            parent->rb_left = right;
        else
            parent->rb_right = right;
    } else
        root->rb_node = right;

    rb_set_parent(node, right);
}

void
rb_rotate_right(struct rb_node *node, struct rb_root *root)
{
    struct rb_node *left   = node->rb_left;
    struct rb_node *parent = rb_get_parent(node);

    if ((node->rb_left = left->rb_right))
        rb_set_parent(left->rb_right, node);

    left->rb_right = node;
    rb_set_parent(left, parent);

    if (parent) {
        if (node == parent->rb_right)
            parent->rb_right = left;
        else
            parent->rb_left = left;
    } else
        root->rb_node = left;

    rb_set_parent(node, left);
}

void
rb_insert_color(struct rb_node *node, struct rb_root *root)
{
    struct rb_node *parent, *uncle;

    while (((parent = rb_get_parent(node))) && rb_is_red(parent)) {
        struct rb_node *gparent = rb_get_parent(parent);

        if (parent == gparent->rb_left) {
            uncle = gparent->rb_right;

            if (uncle && rb_is_red(uncle)) {
                rb_set_black(uncle);
                rb_set_black(parent);
                rb_set_red(gparent);
                node = gparent;
                continue;
            }

            if (parent->rb_right == node) {
                rb_rotate_left(parent, root);
                struct rb_node *tmp = parent;
                parent              = node;
                node                = tmp;
            }

            rb_set_black(parent);
            rb_set_red(gparent);
            rb_rotate_right(gparent, root);
        } else {
            uncle = gparent->rb_left;

            if (uncle && rb_is_red(uncle)) {
                rb_set_black(uncle);
                rb_set_black(parent);
                rb_set_red(gparent);
                node = gparent;
                continue;
            }

            if (parent->rb_left == node) {
                rb_rotate_right(parent, root);
                struct rb_node *tmp = parent;
                parent              = node;
                node                = tmp;
            }

            rb_set_black(parent);
            rb_set_red(gparent);
            rb_rotate_left(gparent, root);
        }
    }

    rb_set_black(root->rb_node);
}

void
rb_erase_color(struct rb_node *node, struct rb_node *parent, struct rb_root *root)
{
    struct rb_node *sibling;

    while ((!node || rb_is_black(node)) && node != root->rb_node) {
        if (parent->rb_left == node) {
            sibling = parent->rb_right;
            if (rb_is_red(sibling)) {
                rb_set_black(sibling);
                rb_set_red(parent);
                rb_rotate_left(parent, root);
                sibling = parent->rb_right;
            }

            if ((!sibling->rb_left || rb_is_black(sibling->rb_left)) &&
                (!sibling->rb_right || rb_is_black(sibling->rb_right))) {
                rb_set_red(sibling);
                node   = parent;
                parent = rb_get_parent(node);
            } else {
                if (!sibling->rb_right || rb_is_black(sibling->rb_right)) {
                    rb_set_black(sibling->rb_left);
                    rb_set_red(sibling);
                    rb_rotate_right(sibling, root);
                    sibling = parent->rb_right;
                }

                rb_set_color(sibling, rb_get_color(parent));
                rb_set_black(parent);
                rb_set_black(sibling->rb_right);
                rb_rotate_left(parent, root);
                node = root->rb_node;
                break;
            }
        } else {
            sibling = parent->rb_left;
            if (rb_is_red(sibling)) {
                rb_set_black(sibling);
                rb_set_red(parent);
                rb_rotate_right(parent, root);
                sibling = parent->rb_left;
            }

            if ((!sibling->rb_left || rb_is_black(sibling->rb_left)) &&
                (!sibling->rb_right || rb_is_black(sibling->rb_right))) {
                rb_set_red(sibling);
                node   = parent;
                parent = rb_get_parent(node);
            } else {
                if (!sibling->rb_left || rb_is_black(sibling->rb_left)) {
                    rb_set_black(sibling->rb_right);
                    rb_set_red(sibling);
                    rb_rotate_left(sibling, root);
                    sibling = parent->rb_left;
                }

                rb_set_color(sibling, rb_get_color(parent));
                rb_set_black(parent);
                rb_set_black(sibling->rb_left);
                rb_rotate_right(parent, root);
                node = root->rb_node;
                break;
            }
        }
    }

    if (node)
        rb_set_black(node);
}

void
rb_erase(struct rb_node *node, struct rb_root *root)
{
    struct rb_node *child, *parent;
    enum rb_color   color;

    if (!node->rb_left) {
        child = node->rb_right;
    } else if (!node->rb_right)
        child = node->rb_left;
    else {
        const struct rb_node *old = node;
        struct rb_node       *left;

        node = node->rb_right;
        while ((left = node->rb_left))
            node = left;

        child  = node->rb_right;
        parent = rb_get_parent(node);
        color  = rb_get_color(node);

        if (child)
            rb_set_parent(child, parent);

        if (parent == old) {
            parent->rb_right = child;
            parent           = node;
        } else
            parent->rb_left = child;

        node->rb_parent_color = old->rb_parent_color;
        node->rb_left         = old->rb_left;
        node->rb_right        = old->rb_right;

        if (rb_get_parent(old)) {
            if (rb_get_parent(old)->rb_left == old)
                rb_get_parent(old)->rb_left = node;
            else
                rb_get_parent(old)->rb_right = node;
        } else
            root->rb_node = node;

        rb_set_parent(old->rb_left, node);
        if (old->rb_right)
            rb_set_parent(old->rb_right, node);

        goto color;
    }

    parent = rb_get_parent(node);
    color  = rb_get_color(node);

    if (child)
        rb_set_parent(child, parent);

    if (parent) {
        if (parent->rb_left == node)
            parent->rb_left = child;
        else
            parent->rb_right = child;
    } else
        root->rb_node = child;

color:
    if (color == RB_BLACK)
        rb_erase_color(child, parent, root);
}

struct rb_node *
rb_first(const struct rb_root *root)
{
    struct rb_node *n = root->rb_node;
    if (!n)
        return NULL;
    while (n->rb_left)
        n = n->rb_left;
    return n;
}

struct rb_node *
rb_last(const struct rb_root *root)
{
    struct rb_node *n = root->rb_node;
    if (!n)
        return NULL;
    while (n->rb_right)
        n = n->rb_right;
    return n;
}

struct rb_node *
rb_next(const struct rb_node *node)
{
    if (node->rb_right) {
        node = node->rb_right;
        while (node->rb_left)
            node = node->rb_left;
        return (struct rb_node *)node;
    }

    struct rb_node *parent = rb_get_parent(node);
    while (parent && node == parent->rb_right) {
        node   = parent;
        parent = rb_get_parent(parent);
    }
    return parent;
}

struct rb_node *
rb_prev(const struct rb_node *node)
{
    if (node->rb_left) {
        node = node->rb_left;
        while (node->rb_right)
            node = node->rb_right;
        return (struct rb_node *)node;
    }

    struct rb_node *parent = rb_get_parent(node);
    while (parent && node == parent->rb_left) {
        node   = parent;
        parent = rb_get_parent(parent);
    }
    return parent;
}
