#ifndef UDX_LINK_H
#define UDX_LINK_H

#define udx__link_add(l, v) \
  if ((l) == NULL) { \
    (v)->next = (v)->prev = NULL; \
  } else { \
    (v)->prev = NULL; \
    (v)->next = (l); \
    (l)->prev = (v); \
  } \
  (l) = (v);

#define udx__link_remove(l, v) \
  if ((v)->next != NULL) (v)->next->prev = (v)->prev; \
  if ((v)->prev == NULL) (l) = (v)->next; \
  else (v)->prev->next = (v)->next;

#define udx__link_foreach(l, el) \
  for ((el) = (l); (el) != NULL; (el) = (el)->next)

#endif
