#ifndef _CONTAINER_QUEUE_H_
#define _CONTAINER_QUEUE_H_

#include <atomic>
namespace common {
namespace router {

#define TSAN_SAFE
#define TSAN_BEFORE(Addr)
#define TSAN_AFTER(Addr)
#define TSAN_ATOMIC(Type) Type

/**
 * Enumerates concurrent queue modes.
 */
enum class EQueueMode
{
	/** Multiple-producers, single-consumer queue. */
	Mpsc,

	/** Single-producer, single-consumer queue. */
	Spsc
};

static inline void* InterlockedExchangePtr(void*volatile* Dest, void* Exchange)
{
	return __sync_lock_test_and_set(Dest, Exchange);
}

inline void MemoryBarrier() {
#if defined(__GLIBCXX__)
  // Work around libstdc++ bug 51038 where atomic_thread_fence was declared but
  // not defined, leading to the linker complaining about undefined references.
  __atomic_thread_fence(std::memory_order_seq_cst);
#else
  std::atomic_thread_fence(std::memory_order_seq_cst);
#endif
}


/**
 * Template for queues.
 *
 * This template implements an unbounded non-intrusive queue using a lock-free linked
 * list that stores copies of the queued items. The template can operate in two modes:
 * Multiple-producers single-consumer (MPSC) and Single-producer single-consumer (SPSC).
 *
 * The queue is thread-safe in both modes. The Dequeue() method ensures thread-safety by
 * writing it in a way that does not depend on possible instruction reordering on the CPU.
 * The Enqueue() method uses an atomic compare-and-swap in multiple-producers scenarios.
 *
 * @param ItemType The type of items stored in the queue.
 * @param Mode The queue mode (single-producer, single-consumer by default).
 * @todo gmp: Implement node pooling.
 */
template<typename ItemType, EQueueMode Mode = EQueueMode::Spsc>
class TQueue
{
public:

	/** Default constructor. */
	TQueue()
	{
		Head = Tail = new TNode();
	}

	/** Destructor. */
	~TQueue()
	{
		while (Tail != nullptr)
		{
			TNode* Node = Tail;
			Tail = Tail->NextNode;

			delete Node;
		}
	}

	/**
	 * Removes and returns the item from the tail of the queue.
	 *
	 * @param OutValue Will hold the returned value.
	 * @return true if a value was returned, false if the queue was empty.
	 * @note To be called only from consumer thread.
	 * @see Empty, Enqueue, IsEmpty, Peek, Pop
	 */
	bool Dequeue(ItemType& OutItem)
	{
		TNode* Popped = Tail->NextNode;

		if (Popped == nullptr)
		{
			return false;
		}
		
		TSAN_AFTER(&Tail->NextNode);
		OutItem = std::move(Popped->Item);

		TNode* OldTail = Tail;
		Tail = Popped;
		Tail->Item = ItemType();
		delete OldTail;

		return true;
	}

	/**
	 * Empty the queue, discarding all items.
	 *
	 * @note To be called only from consumer thread.
	 * @see Dequeue, IsEmpty, Peek, Pop
	 */
	void Empty()
	{
		while (Pop());
	}

	/**
	 * Adds an item to the head of the queue.
	 *
	 * @param Item The item to add.
	 * @return true if the item was added, false otherwise.
	 * @note To be called only from producer thread(s).
	 * @see Dequeue, Pop
	 */
	bool Enqueue(const ItemType& Item)
	{
		TNode* NewNode = new TNode(Item);

		if (NewNode == nullptr)
		{
			return false;
		}

		TNode* OldHead;

		if (Mode == EQueueMode::Mpsc)
		{
			OldHead = (TNode*)InterlockedExchangePtr((void**)&Head, NewNode);
			TSAN_BEFORE(&OldHead->NextNode);
			InterlockedExchangePtr((void**)&OldHead->NextNode, NewNode);
		}
		else
		{
			OldHead = Head;
			Head = NewNode;
			TSAN_BEFORE(&OldHead->NextNode);
			MemoryBarrier();
            		OldHead->NextNode = NewNode;
		}

		return true;
	}

	/**
	 * Adds an item to the head of the queue.
	 *
	 * @param Item The item to add.
	 * @return true if the item was added, false otherwise.
	 * @note To be called only from producer thread(s).
	 * @see Dequeue, Pop
	 */
	bool Enqueue(ItemType&& Item)
	{
		TNode* NewNode = new TNode(std::move(Item));

		if (NewNode == nullptr)
		{
			return false;
		}

		TNode* OldHead;

		if (Mode == EQueueMode::Mpsc)
		{
            		OldHead = (TNode*)InterlockedExchangePtr((void**)&Head, NewNode);
			TSAN_BEFORE(&OldHead->NextNode);
            		InterlockedExchangePtr((void**)&OldHead->NextNode, NewNode);
		}
		else
		{
			OldHead = Head;
			Head = NewNode;
			TSAN_BEFORE(&OldHead->NextNode);
			MemoryBarrier();
			OldHead->NextNode = NewNode;
		}

		return true;
	}

	/**
	 * Checks whether the queue is empty.
	 *
	 * @return true if the queue is empty, false otherwise.
	 * @note To be called only from consumer thread.
	 * @see Dequeue, Empty, Peek, Pop
	 */
	bool IsEmpty() const
	{
		return (Tail->NextNode == nullptr);
	}

	/**
	 * Peeks at the queue's tail item without removing it.
	 *
	 * @param OutItem Will hold the peeked at item.
	 * @return true if an item was returned, false if the queue was empty.
	 * @note To be called only from consumer thread.
	 * @see Dequeue, Empty, IsEmpty, Pop
	 */
	bool Peek(ItemType& OutItem) const
	{
		if (Tail->NextNode == nullptr)
		{
			return false;
		}

		OutItem = Tail->NextNode->Item;

		return true;
	}

	/**
	 * Peek at the queue's tail item without removing it.
	 *
	 * This version of Peek allows peeking at a queue of items that do not allow
	 * copying, such as TUniquePtr.
	 *
	 * @return Pointer to the item, or nullptr if queue is empty
	 */
	ItemType* Peek()
	{
		if (Tail->NextNode == nullptr)
		{
			return nullptr;
		}

		return &Tail->NextNode->Item;
	}

	inline const ItemType* Peek() const
	{
		return const_cast<TQueue*>(this)->Peek();
	}

	/**
	 * Removes the item from the tail of the queue.
	 *
	 * @return true if a value was removed, false if the queue was empty.
	 * @note To be called only from consumer thread.
	 * @see Dequeue, Empty, Enqueue, IsEmpty, Peek
	 */
	bool Pop()
	{
		TNode* Popped = Tail->NextNode;

		if (Popped == nullptr)
		{
			return false;
		}
		
		TSAN_AFTER(&Tail->NextNode);

		TNode* OldTail = Tail;
		Tail = Popped;
		Tail->Item = ItemType();
		delete OldTail;

		return true;
	}

private:

	/** Structure for the internal linked list. */
	struct TNode
	{
		/** Holds a pointer to the next node in the list. */
		TNode* volatile NextNode;

		/** Holds the node's item. */
		ItemType Item;

		/** Default constructor. */
		TNode()
			: NextNode(nullptr)
		{ }

		/** Creates and initializes a new node. */
		explicit TNode(const ItemType& InItem)
			: NextNode(nullptr)
			, Item(InItem)
		{ }

		/** Creates and initializes a new node. */
		explicit TNode(ItemType&& InItem)
			: NextNode(nullptr)
			, Item(std::move(InItem))
		{ }
	};

	/** Holds a pointer to the head of the list. */
	//MS_ALIGN(16) TNode* volatile Head GCC_ALIGN(16);
	TNode* volatile Head;

	/** Holds a pointer to the tail of the list. */
	TNode* Tail;
	
private:

	/** Hidden copy constructor. */
	TQueue(const TQueue&) = delete;

	/** Hidden assignment operator. */
	TQueue& operator=(const TQueue&) = delete;
};
} //namespace utils 
} //namespace xverse
#endif //_CONTAINER_QUEUE_H_

