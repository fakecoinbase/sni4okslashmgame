/*
    author: Ilya Andronov <sni4ok@yandex.ru>
*/

#pragma once

#include "profiler.hpp"

#include <atomic>

template<typename type, uint32_t size>
struct array
{
    type elems[size];
    type& operator[](uint32_t idx) {
        return elems[idx];
    }
    const type& operator[](uint32_t idx) const {
        return elems[idx];
    }
};

//with follower can work one writer and many readers without synchronization
template<typename type, uint32_t block_size = 16384, uint32_t num_blocks = 16384>
class follower : noncopyable
{
    typedef array<type*, num_blocks> bulks_type;
    bulks_type bulks;
    std::atomic<uint32_t> num_elems;
public:
    typedef type value_type;

    follower() : num_elems() {
    }
    ~follower() {
        uint32_t elems = num_elems;
        uint32_t end_bulk = elems / block_size;
        uint32_t end_element = elems % block_size;
        if(end_element)
            ++end_bulk;
        for(uint32_t i = 0; i != end_bulk; ++i)
            delete[] bulks[i];
    }
    void push_back(const type& value) {
        uint32_t elems = num_elems;
        uint32_t end_bulk = elems / block_size;
        uint32_t end_element = elems % block_size;

        if(!end_element) {
            MPROFILE("follower::new")
            bulks[end_bulk] = new type[block_size];
        }

        (bulks[end_bulk])[end_element] = value;
        ++num_elems;
    }
    class const_iterator :
        public std::iterator<std::random_access_iterator_tag, type, int32_t>
    {
        const bulks_type* bulks;
        uint32_t element;

        friend class follower;

        const type& dereference() const { 
            uint32_t end_bulk = element / block_size;
            uint32_t end_element = element % block_size;
            return ((*bulks)[end_bulk])[end_element];
        }
        const_iterator(const bulks_type& bulks, uint32_t element) : bulks(&bulks), element(element){
        }
    public:
        const_iterator(){}
        bool operator==(const const_iterator& r) const {
            return element == r.element;
        }
        bool operator!=(const const_iterator& r) const {
            return element != r.element;
        }
        const_iterator& operator++() {
            ++element;
            return *this;
        }
        const_iterator& operator--() {
            --element;
            return *this;
        }
        const_iterator operator-(int32_t diff) const {
            const_iterator ret(*bulks, element);
            ret -= diff;
            return ret;
        }
        const_iterator operator+(int32_t diff) const {
            const_iterator ret(*bulks, element);
            ret += diff;
            return ret;
        }
        const_iterator& operator-=(int32_t diff) {
            element -= diff;
            return *this;
        }
        const_iterator& operator+=(int32_t diff) {
            element += diff;
            return *this;
        }
        int32_t operator-(const const_iterator& r) const {
            return element - r.element;
        }
        const type& operator*() const {
            return dereference();
        }
        const type* operator->() const {
            const type& t = dereference();
            return &t;
        }
    };
    const_iterator begin() const {
        return const_iterator(bulks, 0);
    }
    const_iterator end() const {
        return const_iterator(bulks, num_elems);
    }
    const type& operator[](uint32_t idx) const {
        return *(begin() + idx);
    }
    uint32_t size() const {
        return num_elems;
    }
};

template<typename type, uint32_t capacity_size>
class lockfree_queue : noncopyable
{
    std::string name;
    struct node
    {
        type elem;
        //0 - empty and free
        //1 - push lock
        //2 - filled and free
        //3 - pop lock
        std::atomic<uint32_t> status;
    };
    array<node, capacity_size> elems;

    std::atomic<uint64_t> push_cnt, pop_cnt;
    static uint64_t get_idx(uint64_t idx) {
        return idx % capacity_size;
    }

    //my_mutex mutex;
    void throw_exception(const char* reason, uint32_t status)
    {
        throw std::runtime_error(es() % name % ":lockfree_queue:" % _str_holder(reason) % ", (" % capacity % "," % pop_cnt % "," % push_cnt % "," % status % ")");
    }
public:
    lockfree_queue(const std::string& name) : name(name)
    {
    }
    static constexpr uint32_t capacity = capacity_size;

    void push(const type& t) {
        push(type(t));
    }
    void push(type&& t) {
        //my_mutex::scoped_lock lock(mutex);
        node& n = elems[get_idx(push_cnt++)];
        uint32_t status = 0;
        if(!n.status.compare_exchange_strong(status, 1))
            throw_exception("push() overloaded", status);
        n.elem = std::move(t);
        status = 1;
        if(!n.status.compare_exchange_strong(status, 2))
            throw_exception("push() internal error", status);
    }
    void pop_strong(type& t) {
        //my_mutex::scoped_lock lock(mutex);
        node& n = elems[get_idx(pop_cnt++)];
        uint32_t status = 2;
        if(!n.status.compare_exchange_strong(status, 3))
            throw_exception("pop_strong() lock error", status);
        t = std::move(n.elem);
        status = 3;
        if(!n.status.compare_exchange_strong(status, 0))
            throw_exception("pop_strong() internal error", status);
    }
    bool pop_weak(type& t) {
        //my_mutex::scoped_lock lock(mutex);
        node& n = elems[get_idx(pop_cnt)];
        uint32_t status = 2;
        if(n.status.compare_exchange_strong(status, 3)) {
            ++pop_cnt;
            t = std::move(n.elem);
            status = 3;
            if(!n.status.compare_exchange_strong(status, 0))
                throw_exception("pop_weak() internal error", status);
            return true;
        }
        return false;
    }
};

template<typename ttype, uint32_t pool_size = 16 * 1024>
struct fast_alloc
{
    typedef ttype type;
    lockfree_queue<type*, pool_size> elems;

    fast_alloc(const std::string& name) : elems(name) {
        for(uint32_t i = 0; i != pool_size; ++i)
            elems.push(new type);
    }
    type* alloc() {
        type* p;
        elems.pop_strong(p);
        return p;
    }
    void free(type* m) {
        elems.push(m);
    }
    ~fast_alloc() {
        type* p;
        while(elems.pop_weak(p))
            delete p;
    }
};

template<typename alloc>
class alloc_holder
{
    alloc &a;
    typedef typename alloc::type type;
    type* p;
public:
    alloc_holder(alloc& a) : a(a), p(a.alloc()) {
    }
    ~alloc_holder() {
        if(p)
            a.free(p);
    }
    type* operator->() {
        return p;
    }
    type& operator*() {
        return *p;
    }
    type* release() {
        type* ret = p;
        p = nullptr;
        return ret;
    }
};

template<typename type>
struct lockfree_list
{
    struct node : type
    {
        node* next;
        node() : next(){}
    };
    lockfree_list() : tail(&root) {
    }
    void push(node* t) {
        node* expected = tail;
        while(!tail.compare_exchange_weak(expected, t)) {
            expected = tail;
        }
        expected->next = t;
    }
    node* begin() {
        return root.next;
    }
    node* next(node* prev) {
        if(!prev)
            return root.next;
        return prev->next;
    }
private:
    node root;
    std::atomic<node*> tail;
};

