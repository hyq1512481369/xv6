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

#define NBUCKETS 13
struct hashbucket{
  struct buf head;
};


struct {
  struct spinlock bucket_lock[NBUCKETS];
  struct spinlock main_lock;
  struct buf buf[NBUF];

  // Linked list of all buffers, through prev/next.
  // Sorted by how recently the buffer was used.
  // head.next is most recent, head.prev is least.
  struct hashbucket buckets[NBUCKETS];
} bcache;

static char lock_name[NBUCKETS][9];
void
binit(void)
{
  struct buf *b;
 
  initlock(&bcache.main_lock, "main_lock");
  for(int i=0;i<NBUCKETS;i++){
    strncpy(lock_name[i],"bcacheN\0\0",8);
    if(i<10)lock_name[i][6]='0'+i;
    else {lock_name[i][6]='1';lock_name[i][7]=i-10+'0';}
    initlock(&(bcache.bucket_lock[i]),lock_name[i]);
    // Create linked list of buffers
    bcache.buckets[i].head.prev =  &bcache.buckets[i].head;
    bcache.buckets[i].head.next =  &bcache.buckets[i].head;
  }
    for(b = bcache.buf; b < bcache.buf+NBUF; b++){
      int hash=(b-bcache.buf)%NBUCKETS;
      b->next = bcache.buckets[hash].head.next;
      b->prev = &bcache.buckets[hash].head;
      initsleeplock(&b->lock, "buffer");
      bcache.buckets[hash].head.next->prev = b;
      bcache.buckets[hash].head.next = b;
    }
  
}

// Look through buffer cache for block on device dev.
// If not found, allocate a buffer.
// In either case, return locked buffer.
static struct buf*
bget(uint dev, uint blockno)
{
  struct buf *b;
  int hash=blockno%NBUCKETS;
  acquire(&bcache.bucket_lock[hash]);

  // Is the block already cached?
  for(b = bcache.buckets[hash].head.next; b != &bcache.buckets[hash].head; b = b->next){
    
    if(b->dev == dev && b->blockno == blockno){
      b->refcnt++;
      release(&bcache.bucket_lock[hash]);
      acquiresleep(&b->lock);
      return b;
    }
  }
   for(b = bcache.buckets[hash].head.next; b != &bcache.buckets[hash].head; b = b->next){
    
    if(b->refcnt==0){
        b->dev = dev; 
        b->blockno = blockno;
        b->valid = 0;
        b->refcnt = 1;
      release(&bcache.bucket_lock[hash]);
      acquiresleep(&b->lock);
      return b;
    }
  }

 release(&bcache.bucket_lock[hash]);
  // Not cached.
  // Recycle the least recently used (LRU) unused buffer.
  acquire(&bcache.main_lock);
  acquire(&bcache.bucket_lock[hash]);
  for(b = bcache.buckets[hash].head.next; b != &bcache.buckets[hash].head; b = b->next){
    if(b->dev == dev && b->blockno == blockno){
      b->refcnt++;
      release(&bcache.bucket_lock[hash]);
      release(&bcache.main_lock);
      acquiresleep(&b->lock);
      return b;
    }
  }
   for(b = bcache.buckets[hash].head.next; b != &bcache.buckets[hash].head; b = b->next){
    if(b->refcnt==0){
        b->dev = dev; 
        b->blockno = blockno;
        b->valid = 0;
        b->refcnt = 1;
      release(&bcache.bucket_lock[hash]);
      release(&bcache.main_lock);
      acquiresleep(&b->lock);
      return b;
    }
  }
  for(int k=hash+1;k<hash+NBUCKETS;k++){
    int temp=k%NBUCKETS;
    acquire(&bcache.bucket_lock[temp]);
    for(b = bcache.buckets[temp].head.prev; b != &bcache.buckets[temp].head; b = b->prev){
      
      
      if(b->refcnt == 0) {
        b->dev = dev; 
        b->blockno = blockno;
        b->valid = 0;
        b->refcnt = 1;
        b->prev->next=b->next;
        b->next->prev=b->prev;
        b->next=bcache.buckets[hash].head.next;
        b->prev=b->next->prev;
        b->prev->next=b;
        b->next->prev=b;
        if(hash!=temp)release(&bcache.bucket_lock[temp]);
        release(&bcache.bucket_lock[hash]);
        release(&bcache.main_lock);
        acquiresleep(&b->lock);
        return b;
      }
    }
    release(&bcache.bucket_lock[temp]);
  }
  panic("bget: no buffers");
}

// Return a locked buf with the contents of the indicated block.
struct buf*
bread(uint dev, uint blockno)
{
  struct buf *b;

  b = bget(dev, blockno);
  if(!b->valid) {
    virtio_disk_rw(b, 0);
    b->valid = 1;
  }
  return b;
}

// Write b's contents to disk.  Must be locked.
void
bwrite(struct buf *b)
{
  if(!holdingsleep(&b->lock))
    panic("bwrite");
  virtio_disk_rw(b, 1);
}

// Release a locked buffer.
// Move to the head of the most-recently-used list.
void
brelse(struct buf *b)
{
  if(!holdingsleep(&b->lock))
    panic("brelse");
  
  releasesleep(&b->lock);

  int hash=b->blockno%NBUCKETS;
  acquire(&bcache.bucket_lock[hash]);
  b->refcnt--;
  if (b->refcnt == 0) {
    // no one is waiting for it.
    b->next->prev = b->prev;
    b->prev->next = b->next;
    b->next = bcache.buckets[hash].head.next;
    b->prev = &bcache.buckets[hash].head;
    bcache.buckets[hash].head.next->prev = b;
    bcache.buckets[hash].head.next = b;
  }
  
  release(&bcache.bucket_lock[hash]);
}

void
bpin(struct buf *b) {
  int hash=b->blockno%NBUCKETS;
  acquire(&bcache.bucket_lock[hash]);
  b->refcnt++;
  release(&bcache.bucket_lock[hash]);
}

void
bunpin(struct buf *b) {
   int hash=b->blockno%NBUCKETS;
  acquire(&bcache.bucket_lock[hash]);
  b->refcnt--;
  release(&bcache.bucket_lock[hash]);
}


