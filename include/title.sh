#!/bin/bash

if [ "$#" -ne 2 ]; then
  echo "Usage: $0 <file_path> <name>"

  exit 1
fi


FILE_PATH="$1"

TITLE="$2"


if [ ! -f "$FILE_PATH" ]; then
  echo "Error: File '$FILE_PATH' does not exist."
  exit 1
fi


TEMP_FILE=$(mktemp)


echo "/* @title: $TITLE */" > "$TEMP_FILE"
cat "$FILE_PATH" >> "$TEMP_FILE"

mv "$TEMP_FILE" "$FILE_PATH"

echo "Title added successfully to '$FILE_PATH'."
