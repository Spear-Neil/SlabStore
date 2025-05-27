#include <iostream>
#include <thread>

class Demo {
  int a;

 public:
  Demo() = default;

  ~Demo() = default;
};

int main() {
  for(int i = 0; i < 10; i++) {
    if(i % 2 == 0) continue;
    std::cout << i << std::endl;
  }

  return 0;
}