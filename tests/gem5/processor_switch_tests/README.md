# Processor Switch

These tests check processor switching in gem5.

To run the FS variant of these tests by themselves, you can run the
following command in the tests directory:

```bash
./main.py run gem5/processor_switch_tests --length=very-long --variant=fast
```

To run the SE variant of these tests, you can run the following:

```bash
./main.py run gem5/processor_switch_tests --length=long --variant=fast
```
