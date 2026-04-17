#!/bin/bash

# Check if exactly two arguments are provided
if [ $# -ne 2 ]; then
    echo "Error: Two arguments required: <writefile> <writestr>"
    exit 1
fi

# Assign arguments to variables
writefile=$1
writestr=$2

# Get the directory path
dir=$(dirname "$writefile")

# Create the directory if it doesn't exist
mkdir -p "$dir"
if [ $? -ne 0 ]; then
    echo "Error: Could not create directory $dir"
    exit 1
fi

# Write the string to the file, overwriting if it exists
echo "$writestr" > "$writefile"
if [ $? -ne 0 ]; then
    echo "Error: Could not write to file $writefile"
    exit 1
fi