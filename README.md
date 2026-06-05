# RCU — Read Copy Update

---

## What is RCU?

RCU (Read Copy Update) is a mechanism for allowing **fast reading** and **occasional updating** of shared data structures — without requiring locks for readers.

> Core idea: readers never lock. Only writers need synchronization with each other.

---


## Example 1: Inserting at the Head

List starts as: `HEAD → [key:2, val:1] → NULL`

We want to insert `[key:5, val:3]` at the head.

```
Phase 1 — Original
  HEAD → [K:2, V:1] → NULL
  Readers see: 1-node list

Phase 2 — malloc() new node
  HEAD → [K:2, V:1] → NULL   (new node uninitialized, not linked)
  Readers see: 1-node list

Phase 3 — Initialize new node, set new_node.next = HEAD
  [K:5, V:3] → [K:2, V:1] → NULL   (HEAD not yet updated)
  Readers see: 1-node list

Phase 4 — Atomically update HEAD to new node
  HEAD → [K:5, V:3] → [K:2, V:1] → NULL
  Readers see: 1-node list (old) OR 2-node list (new)
```



## Example 2: Deleting a Node

List: `HEAD → [A] → [B] → [C] → NULL`  
Goal: delete node B.

```
Phase 1 — Original
  HEAD → [A] → [B] → [C] → NULL
  Readers see: 3-node list

Phase 2 — Change A's next pointer to skip B (A → C)
  HEAD → [A] → [C] → NULL     (B still in memory, just bypassed)
  New readers see: 2-node list
  Concurrent readers see: 2 OR 3 nodes (depends on where they are)

Phase 3 — Wait for all readers active at Phase 2 to finish
  Readers see: 2-node list (all old readers done)

Phase 4 — free(B)
  B is safely reclaimed — no reader can be looking at it
```

> ⚠️ You CANNOT free B immediately after changing the pointer.
> Some readers may still hold a reference to B and be mid-traversal.
> You must wait until ALL those readers have finished.

---

## Example 3: Modifying a Node (The "Copy" in RCU)

Goal: change `[key:7, val:3]` to `[key:17, val:13]`.

**Why not update in-place?**  
Two fields must change. Updating one-at-a-time risks a reader seeing `[key:17, val:3]` or `[key:7, val:13]` — both inconsistent.

**Solution — Copy then swap:**

```
Phase 1 — Original
  ... → [K:7, V:3] → ...
  Readers see: key=7, val=3

Phase 2 — malloc() new node
  Readers see: key=7, val=3

Phase 3 — Initialize new node (key=17, val=13, next = old.next)
  Readers see: key=7, val=3

Phase 4 — Atomically update predecessor's pointer to new node
  ... → [K:17, V:13] → ...     [K:7, V:3] still in memory
  New readers see: key=17, val=13
  Old readers see: key=7 or 17 (depends on whether they've read the pointer yet)

Phase 5 — Wait for existing readers (those active at Phase 4)
  All readers see: key=17, val=13

Phase 6 — free(old node)
  Old node safely reclaimed
```

```
Before:   predecessor → [K:7,  V:3 ] → rest...
After:    predecessor → [K:17, V:13] → rest...
                              ↑ old node still exists until Phase 6
```

---





## What is a Grace Period?

A grace period is the window of time the writer waits after making a 
structural change, before it is safe to free memory.

                    splice-out        free
                         │              │
time ────────────────────┼──────────────┼──────────►
                         │◄────────────►│
                           grace period

Any reader that started BEFORE the splice-out must finish before 
the grace period ends. Any reader that starts AFTER the splice-out 
will never see the old pointer.

synchronize_rcu() is the call that waits for the grace period to elapse.

## Read-Side Critical Section

Readers wrap traversal in read_lock/read_unlock:

    rcu.read_lock();
    Node* n = head.load(std::memory_order_acquire);
    while (n) {
        if (n->value == target) {
            process(n->value);   // safe — node cannot be freed here
            break;
        }
        n = rcu_dereference(n->next);
    }
    rcu.read_unlock();

Between read_lock and read_unlock:
- No locks are held
- Other readers run concurrently with zero contention
- Writers may modify the list — but cannot free any node yet
- The reader may see the old structure OR the new one — both are valid
