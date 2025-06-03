#include <stddef.h>

extern "C" {
void *kmalloc(unsigned long size);
void kfree(void *ptr);
}

void *operator new(size_t size) {
    return kmalloc(size);
}

void *operator new[](size_t size) {
    return kmalloc(size);
}

void operator delete(void *ptr) noexcept {
    kfree(ptr);
}

void operator delete[](void *ptr) noexcept {
    kfree(ptr);
}

template <typename T> class ReallyBadVec {
  public:
    ReallyBadVec(size_t initial_capacity = 4)
        : size_(0), capacity_(initial_capacity) {
        data_ = new T[capacity_];
    }

    ~ReallyBadVec() { delete[] data_; }

    void push_back(const T &value) {
        if (size_ >= capacity_) {
            resize(capacity_ * 2);
        }
        data_[size_++] = value;
    }

    T &operator[](size_t index) { return data_[index]; }

    size_t size() const { return size_; }

  private:
    void resize(size_t new_capacity) {
        T *new_data = new T[new_capacity];
        for (size_t i = 0; i < size_; ++i)
            new_data[i] = data_[i];

        delete[] data_;
        data_ = new_data;
        capacity_ = new_capacity;
    }

    T *data_;
    size_t size_;
    size_t capacity_;
};

class TerribleThing {
  public:
    TerribleThing() : value(0) {}
    TerribleThing(int v) : value(v) {}
    int get() const { return value; }

  private:
    int value;
};

extern "C" void cpp_test() {
    ReallyBadVec<TerribleThing> vec;

    for (int i = 0; i < 5; ++i) {
        vec.push_back(TerribleThing(i * 10));
    }

    for (size_t i = 0; i < vec.size(); ++i) {
        int val = vec[i].get();
        extern void k_printf(const char *, ...);
        k_printf("vec[%zu] = %d\n", i, val);
    }
}
