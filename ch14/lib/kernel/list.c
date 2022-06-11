#include "list.h"
//涉及到修改链表的操作都要关中断
#include "interrupt.h"
//初始化链表
void list_init(struct list* list) {
    list->head.prev = NULL;
    list->head.next = &list->tail;
    list->tail.prev = &list->head;
    list->tail.next = NULL;
}

//把链表元素elem插入在before之前
void list_insert_before(struct list_elem* before, struct list_elem* elem) {
    enum intr_status old_status = intr_disable();
    
    before->prev->next = elem;
    elem->prev = before->prev;
    elem->next = before; 
    before->prev = elem;

    intr_set_status(old_status);
}

// 添加元素到队列首，类似栈的push操作
void list_push(struct list* plist, struct list_elem* elem) {
    list_insert_before(plist->head.next, elem);
}

// 追加元素到队列队尾，类似队列的先进先出操作
void list_append(struct list* plist, struct list_elem* elem) {
    list_insert_before(&plist->tail, elem);
}

// 使元素pelem脱离链表
void list_remove(struct list_elem* pelem) {
    enum intr_status old_status = intr_disable();

    pelem->prev->next = pelem->next;
    pelem->next->prev = pelem->prev;

    intr_set_status(old_status);
}

// 将链表的第一个元素弹出并返回，类似栈的pop操作
struct list_elem* list_pop(struct list* plist) {
    struct list_elem* elem = plist->head.next;
    list_remove(elem);
    return elem;
}

// 从链表中查找元素 object_elem, 成功时返回true，失败时返回false
bool elem_find(struct list* plist, struct list_elem* obj_elem) {
    struct list_elem* elem = plist->head.next;
    while(elem != &plist->tail) {
        if(elem == obj_elem) {
            return true;
        }
        elem = elem->next;
    }
    return false;
}

// 判断链表是否为空
bool list_empty(struct list* plist) {
    return (plist->head.next == &plist->tail);
}

// 返回链表长度
uint32_t list_len(struct list* plist) {
    struct list_elem* elem = plist->head.next;
    uint32_t length = 0;
    while(elem != &plist->tail) {
        length++;
        elem = elem->next;
    }
    return length;
}

//遍历列表，将链表中的每个元素elem和arg都传给回调函数func
// 使用func判断节点是否满足某个要求，找到一个满足的元素就返回这个元素，如果整个两表都没有满足条件的节点，那就返回NULL
struct list_elem* list_traversal(struct list* plist, function func, int arg) {
    struct list_elem* elem = plist->head.next;
    // 如果列表为空，就必然没有符合条件的节点，故直接返回空
    if(list_empty(plist)) {
        return NULL;
    }

    while (elem != &plist->tail)
    {
        if(func(elem, arg)) {
            return elem;
        }
        elem = elem->next;
    }
    return NULL;
    
}