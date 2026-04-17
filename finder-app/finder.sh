#!/bin/bash

# Check if exactly two arguments are provided
if [ $# -ne 2 ]; then
    echo "Error: Two arguments required: <filesdir> <searchstr>"
    exit 1
fi

# Assign arguments to variables
filesdir=$1
searchstr=$2

# Check if filesdir is a directory
if [ ! -d "$filesdir" ]; then
    echo "Error: $filesdir is not a directory"
    exit 1
fi

# Count the number of files in the directory and subdirectories
X=$(find "$filesdir" -type f | wc -l)

# Count the number of matching lines containing searchstr
Y=$(grep -r "$searchstr" "$filesdir" | wc -l)

# Print the result
echo "The number of files are $X and the number of matching lines are $Y"