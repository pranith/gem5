#include <stdio.h>

int
func1(int* arg)
{
  *arg = *arg + 1;

  return 1;
}

int
func2(int* arg)
{
  *arg = *arg + 2;

  return 1;
}

int main()
{
  int count = 0, calls = 0;
  int (*f_ptr)(int*);
  for (int i = 0; i < 100; i++) {
    if (i % 7 == 0) {
      f_ptr = func2;
    } else {
      f_ptr = func1;
    }

    calls += (*f_ptr)(&count);
  }

  printf("calls:%d, count:%d\n", calls, count);

  return 0;
}
