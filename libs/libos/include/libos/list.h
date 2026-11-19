/*
 * libos/list.h - intrusive circular doubly-linked list.
 * Embed a struct list_node in your struct; the list head is also a list_node.
 */
#ifndef LIBOS_LIST_H
#define LIBOS_LIST_H

#include <stdbool.h>
#include <stddef.h>

struct list_node {
    struct list_node *prev;
    struct list_node *next;
};

/* Pointer to the struct that contains `node` as field `member`. */
#define LIST_ENTRY(node, type, member) \
    ((type *)(void *)((char *)(node) - offsetof(type, member)))

static inline void list_init(struct list_node *head)
{
    head->prev = head;
    head->next = head;
}

static inline bool list_empty(const struct list_node *head)
{
    return head->next == head;
}

static inline void list_push(struct list_node *head, struct list_node *node)
{
    node->next = head->next;
    node->prev = head;
    head->next->prev = node;
    head->next = node;
}

static inline void list_remove(struct list_node *node)
{
    node->prev->next = node->next;
    node->next->prev = node->prev;
    node->prev = node;
    node->next = node;
}

/* Removes and returns the first node, or NULL if the list is empty. */
static inline struct list_node *list_pop(struct list_node *head)
{
    if (list_empty(head)) {
        return NULL;
    }
    struct list_node *node = head->next;
    list_remove(node);
    return node;
}

#endif /* LIBOS_LIST_H */
