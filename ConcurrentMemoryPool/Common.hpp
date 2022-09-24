#pragma once
#include <iostream>
#include <assert.h>
#include <thread>
#include <mutex>
#include <vector>
#include <unordered_map>

#ifdef _WIN32
#include <windows.h>
#endif // _WIN32

const size_t NLISTS = 240;   //管理自由链表数组的长度,根据对齐规则计算出来的

const size_t MAXBYTES = 64 * 1024; //ThreadCache最大可以一次分配多大的内存64K

const size_t PAGE_SHIFT = 12;//一页是4K，即4096字节,2的12次方=4096

const size_t NPAGES = 129;   //PageCache的最大可以存放NPAGES-1页


// 向系统申请内存
static inline void* SystemAlloc(size_t npage)//以页为单位向系统内存申请内存

{
#ifdef _WIN32
	void* ptr = VirtualAlloc(NULL, (npage) << PAGE_SHIFT, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);//使用VirtualAlloc()创建虚拟内存，修改可读、可写属性，如果调用成功,返回分配的首地址

	if (ptr == nullptr)
	{
		throw std::bad_alloc();//抛出异常
	}

	return ptr;
#else 
#endif //_WIN32
}

//释放内存
static inline void SystemFree(void* ptr)
{
#ifdef _WIN32
	VirtualFree(ptr, 0, MEM_RELEASE);
	if (ptr == nullptr)
	{
		throw std::bad_alloc();
	}
#else 
#endif //_WIN32
}


// 取出该指针的前4或者前8个字节
//NEXT_OBJ取得的是内存块的前边4字节或者8字节。
//比如一个8byte大小内存块，他的开始内存地址是0x00FF0001，那么从这个地址开始后的4个byte大小就是指向下一个内存块，同时这4个byte也是属于那8个byte中的。
static inline void*& NEXT_OBJ(void* obj)//指针的本身就表示地址，所以传入的void *obj就是地址，抢取对象头四个或者头八个字节，void*的别名，本身是内存，只能我们自己取
{
	return *((void**)obj);//先强转为void**,然后解引用就是一个void*，所以最后返回的就是一直指向地址的指针。
}

// 自由链表类
class FreeList
{
public:
	bool Empty()
	{
		return _list == nullptr;
	}

	// 将一段内存对象添加到自由链表当中
	void PushRange(void* start, void* end, size_t num)//取出来的这n个对象是链接在一起的，我们只需要得到这段链表的头和尾即可。SZC个人认为这里的star和end不是想象中的一个地址，而是链表的头结点和尾结点。
	{
		//_list->start->end->_list//这样PushRange、Pop、Push三个函数对链表的操作就可以一致了。
		NEXT_OBJ(end) = _list;
		_list = start;
		_size += num;
	}

	//头删
	void* Pop()
	{
		void* obj = _list;
		_list = NEXT_OBJ(obj);
		_size--;

		return obj;
	}

	//头插
	void Push(void* ptr)
	{
		NEXT_OBJ(ptr) = _list;
		_list = ptr;
		_size++;
	}

	size_t MaxSize()
	{
		return _maxsize;
	}

	void SetMaxSize(size_t num)
	{
		_maxsize = num;
	}

	size_t Size()
	{
		return _size;
	}

	void* Clear()
	{
		_size = 0;
		void* list = _list;
		_list = nullptr;

		return list;
	}

private:
	void* _list = nullptr;// 一个自由链表
	size_t _size = 0;     // 内存结点个数
	size_t _maxsize = 1;  // 最多有多少个内存结点 作用:水位线
};

// 使用span数据结构管理内存
// 一个span包含多页
typedef size_t PageID;
struct Span
{
	PageID _pageid = 0;  //页号
	size_t _npage = 0;   //页的数量

	// 维护一条span对象的双向带头循环链表
	Span* _next = nullptr;
	Span* _prev = nullptr;

	void* _objlist = nullptr; // 内存对象自由链表
	size_t _objsize = 0;	  // 记录该span上的内存块的大小
	size_t _usecount = 0;     // 使用计数
};

// 跨度链表类，双向链表
class SpanList
{
public:
	SpanList()
	{
		_head = new Span;
		_head->_next = _head;
		_head->_prev = _head;
	}
//给SpanList类提供Begin和End成员函数，分别用于获取双链表中的第一个span(说明_head只是一个头指针)和最后一个span的下一个位置，也就是头结点。
	Span* begin()
	{
		return _head->_next;
	}
	
	Span* end()
	{
		return _head;
	}

	bool Empty()
	{
		return _head == _head->_next;
	}

	void Insert(Span* cur, Span* newspan)
	{
		assert(cur);
		Span* prev = cur->_prev;
		
		//prev newspan cur
		prev->_next = newspan;
		newspan->_prev = prev;
		newspan->_next = cur;
		cur->_prev = newspan;
	}

	void Erase(Span* cur)
	{
		assert(cur != nullptr && cur != _head);

		Span* prev = cur->_prev;
		Span* next = cur->_next;

		prev->_next = next;
		next->_prev = prev;
	}

	void PushBack(Span* cur)
	{
		Insert(end(), cur);
	}

	void PopBack()
	{
		Span* span = end();
		Erase(span);
	}

	void PushFront(Span* cur)
	{
		Insert(begin(), cur);
	}

	Span* PopFront()
	{
		Span* span = begin();
		Erase(span);

		return span;
	}

	// 为了给每一个桶加锁
	std::mutex _mtx;
private:
	Span * _head = nullptr;
};

// 大小类,管理对齐和映射等关系
class ClassSize
{
public:
	// align是对齐数
	static inline size_t _RoundUp(size_t size, size_t align)//找到申请的内存大小对齐后的大小。比如申请25byte的内存，25<128,则按8字节对齐，所以实际应该分配(((25+7)/8)*8)的大小即32。
	{
		// 比如size是15 < 128,对齐数align是8，那么要进行向上取整，
		// ((15 + 7) / 8) * 8就可以了
		// 这个式子就是将(align - 1)加上去
		// 然后再将加上去的二进制的低三位设置为0
		// 15 + 7 = 22 : 10110 
		// 7 : 111 ~7 : 000
		// 22 & ~7 : 10000 (16)就达到了向上取整的效果
		return (size + align - 1) & ~(align - 1);
	}
	// 向上取整
	static inline size_t RoundUp(size_t size)
	{
		assert(size <= MAXBYTES);
		
		if (size <= 128)
		{
			return _RoundUp(size, 8);
		}
		if (size <= 8 * 128)
		{
			return _RoundUp(size, 16);
		}
		if (size <= 8 * 1024)
		{
			return _RoundUp(size, 128);
		}
		if (size <= 64 * 1024)
		{
			return _RoundUp(size, 512);
		}
		else
		{
			return -1;
		}
	}

	//控制内碎片在12%左右的浪费
	//[1, 128]						8byte对齐		freelist[0,16)
	//[129, 1024]					16byte对齐		freelist[17, 72)
	//[1025, 8 * 1024]				64byte对齐		freelist[72, 128)
	//[8 * 1024 + 1, 64 * 1024]		512byte对齐		freelist[128, 240)
	//也就是说自由链表数组只需要开辟240个空间就可以了

	//求出在该区间的第几个
	static size_t _Index(size_t bytes, size_t align_shift)
	{
		// 给bytes加上对齐数减一也就是
		// 让其可以跨越到下一个自由链表的数组的元素中
		return ((bytes + (1 << align_shift) - 1) >> align_shift) - 1;
	}

	//获取自由链表的下标
	static inline size_t Index(size_t bytes)
	{
		assert(bytes < MAXBYTES);

		// 记录每个对齐区间中有着多少条自由链表
		static int group_array[4] = { 16, 56, 56, 112};

		if (bytes <= 128)
		{
			return _Index(bytes, 3);//8字节对齐，即2^3
		}
		else if (bytes <= 1024) //(8 * 128)
		{
			return _Index(bytes - 128, 4) + group_array[0];//16字节对齐，即2^4
		}
		else if (bytes <= 4096) //(8 * 8 * 128)
		{
			return _Index(bytes - 1024, 7) + group_array[1] + group_array[0];//128字节对齐，即2^7
		}
		else if (bytes <= 8 * 128)
		{
			return _Index(bytes - 4096, 9) + group_array[2] + group_array[1] + group_array[0];//字节对齐，即2^9
		}
		else
		{
			return -1;
		}
	}

	// 计算一次从中心缓存中移动多少个内存对象到ThreadCache中
	static inline size_t NumMoveSize(size_t size)
	{
		if (size == 0)
		{
			return 0;
		}

		int num = (int)(MAXBYTES / size);

		if (num < 2)
		{
			num = 2;
		}
		if (num >= 512)
		{
			num = 512;
		}

		return num;
	}

	// 根据size计算中心缓存要从页缓存中取多大的span对象
	static inline size_t NumMovePage(size_t size)
	{
		size_t num = NumMoveSize(size);
		size_t npage = (num * size) >> PAGE_SHIFT;

		if (npage == 0)
		{
			npage = 1;
		}

		return npage;
	}

};