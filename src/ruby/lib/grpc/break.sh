#!/bin/bash
while [ $? == 0 ]
do
  LD_LIBRARY_PATH=. ./a.out
done

