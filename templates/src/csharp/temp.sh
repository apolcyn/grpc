#!/bin/bash
for d in */ ; do
  cp $(basename $d)/project.json.template $(basename $d)/$(basename $d).project.json.template
  rm $(basename $d)/$(basename $d).json.template
  rm $(basename $d)/$(basename $d).json
done
