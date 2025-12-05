#!/usr/bin/env python3

import sys

def main():
    # Check if correct number of arguments are provided
    if len(sys.argv) != 3:
        print("Usage: ./script.py STRING file.c")
        sys.exit(1)

    # Get the arguments
    string = sys.argv[1]
    file_path = sys.argv[2]

    try:
        # Read the contents of the file
        with open(file_path, 'r') as file:
            content = file.read()

        # Add the #ifdef and #endif around the content
        updated_content = f"#ifdef {string}\n\n{content}\n#endif\n"

        # Write the updated content back to the file
        with open(file_path, 'w') as file:
            file.write(updated_content)

        print(f"Updated file '{file_path}' successfully.")

    except FileNotFoundError:
        print(f"Error: File '{file_path}' not found.")
        sys.exit(1)

    except Exception as e:
        print(f"An error occurred: {e}")
        sys.exit(1)

if __name__ == "__main__":
    main()
