#!/bin/sh

./build/ARM/gem5.debug -d debug --debug-flags=O3CPUAll configs/example/arm/rancho_o3.py test --cpu rancho --mem-size 8GB
