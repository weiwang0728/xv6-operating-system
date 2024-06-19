// Buffer cache.
//
// The buffer cache is a linked list of buf structures holding
// cached copies of disk block contents.  Caching disk blocks
// in memory reduces the number of disk reads and also provides
// a synchronization point for disk blocks used by multiple processes.
//
// Interface:
// * To get a buffer for a particular disk block, call bread.
// * After changing buffer data, call bwrite to write it to disk.
// * When done with the buffer, call brelse.
// * Do not use the buffer after calling brelse.
// * Only one process at a time can use a buffer,
//     so do not keep them longer than necessary.


#include "types.h"
#include "param.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "riscv.h"
#include "defs.h"
#include "fs.h"
#include "buf.h"

// struct {
//   struct spinlock lock;
//   struct buf buf[NBUF];

//   // Linked list of all buffers, through prev/next.
//   // Sorted by how recently the buffer was used.
//   // head.next is most recent, head.prev is least.
//   struct buf head;
// } bcache;

#define NHASH 13
struct {
  struct spinlock lock;
  struct buf head;
} hash_table[NHASH];

int hash(int blockno) {
  return blockno % NHASH;
}

struct spinlock freelist_lock;
struct buf freelist_head;
struct buf cache[NBUF];

void binit() {
  struct buf *b;
  for (int i = 0; i < NHASH; i++) {
    initlock(&hash_table[i].lock, "hash bucket lock");
    hash_table[i].head.prev = &hash_table[i].head;
    hash_table[i].head.next = &hash_table[i].head;
  }

  initlock(&freelist_lock, "freelist lock");
  freelist_head.next = &freelist_head;
  freelist_head.prev = &freelist_head;

  for (b = cache; b < cache + NBUF; b++) {
    initsleeplock(&b->lock, "buffer");
    b->next = freelist_head.next;
    b->prev = &freelist_head;
    freelist_head.next->prev = b;
    freelist_head.next = b;
  }
}

static struct buf* bget(uint dev, uint blockno) {
  struct buf *b;
  int h = hash(blockno);

  acquire(&hash_table[h].lock);
  for (b = hash_table[h].head.next; b != &hash_table[h].head; b = b->next) {
    if (b->dev == dev && b->blockno == blockno) {
      b->refcnt++;
      release(&hash_table[h].lock);
      acquiresleep(&b->lock);
      return b;
    }
  }

  acquire(&freelist_lock);
  for (b = freelist_head.next; b != &freelist_head; b = b->next) {
    if (b->refcnt == 0) {
      b->next->prev = b->prev;
      b->prev->next = b->next;
      release(&freelist_lock);

      b->dev = dev;
      b->blockno = blockno;
      b->valid = 0;
      b->refcnt = 1;
      b->next = hash_table[h].head.next;
      b->prev = &hash_table[h].head;
      hash_table[h].head.next->prev = b;
      hash_table[h].head.next = b;
      release(&hash_table[h].lock);
      
      acquiresleep(&b->lock);
      return b;
    }
  }
  release(&freelist_lock);

  panic("bget: no buffers");
}

struct buf* bread(uint dev, uint blockno) {
  struct buf *b = bget(dev, blockno);
  if (!b->valid) {
    virtio_disk_rw(b, 0);
    b->valid = 1;
  }
  return b;
}

void bwrite(struct buf *b) {
  if (!holdingsleep(&b->lock)) panic("bwrite");
  virtio_disk_rw(b, 1);
}

void brelse(struct buf *b) {
  if (!holdingsleep(&b->lock)) panic("brelse");

  releasesleep(&b->lock);

  int h = hash(b->blockno);
  acquire(&hash_table[h].lock);
  b->refcnt--;
  if (b->refcnt == 0) {
    b->next->prev = b->prev;
    b->prev->next = b->next;

    acquire(&freelist_lock);
    b->next = freelist_head.next;
    b->prev = &freelist_head;
    freelist_head.next->prev = b;
    freelist_head.next = b;
    release(&freelist_lock);
  }
  release(&hash_table[h].lock);
}

void bpin(struct buf *b) {
  int h = hash(b->blockno);
  acquire(&hash_table[h].lock);
  b->refcnt++;
  release(&hash_table[h].lock);
}

void bunpin(struct buf *b) {
  int h = hash(b->blockno);
  acquire(&hash_table[h].lock);
  b->refcnt--;
  release(&hash_table[h].lock);
}
