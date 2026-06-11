#!/bin/bash

if [ "$#" -eq 0 ]; then
    echo "usage: $0 files..."
    echo "    files    files with sensor measurements stored in CSV format"
    echo ""
    echo "Replaces the timestamps in the first column of files with timestamps"
    echo "incremented every 12 lines, starting from the current system time."
    echo "Modified files are saved with a '_fixed' suffix after their basename."
fi

for F in $@
do
printf "Fixing $F ..." && \
    cat $F \
        | awk -F, '{ print $1 ~ /^[0-9]{4}-[01][0-9]-[0-3][0-9]T[0-2][0-9]:[0-5][0-9]:[0-5][0-9]Z/ ? strftime("%FT%TZ", systime() + (NR - 2) / 12) "," $2 "," $3 "," $4 : $0 }' \
        > "${F%.*}_fixed.${F#*.}" && \
    printf "OK\nFixed version in ${F%.*}_fixed.${F#*.}.\n\n"
done
