import os
import sys
import shutil
import re

def move_and_update(old_path, new_path):
    new_dir = os.path.dirname(new_path)
    if new_dir and not os.path.exists(new_dir):
        os.makedirs(new_dir)

    shutil.move(old_path, new_path)
    print(f"Moved: {old_path} -> {new_path}")

    old_filename = os.path.basename(old_path)
    new_path_norm = new_path.replace("\\", "/")

    base_dir = os.path.abspath(os.path.join(os.getcwd(), ".."))

    pattern = re.compile(r'[<"]([^">]+)[>"]')

    for root, dirs, files in os.walk(base_dir):
        for file in files:
            full_path = os.path.join(root, file)

            if not file.endswith(('.c', '.h', '.cpp', '.hpp', '.txt', '.inl')):
                continue

            try:
                with open(full_path, 'r', encoding='utf-8') as f:
                    content = f.read()
            except (UnicodeDecodeError, OSError):
                continue  

            updated = False

            def replace_include(match):
                nonlocal updated
                inner = match.group(1)
                if os.path.basename(inner) == old_filename:
                    updated = True
                    return f'<{new_path_norm}>'
                return match.group(0)

            new_content = pattern.sub(replace_include, content)

            if updated:
                with open(full_path, 'w', encoding='utf-8') as f:
                    f.write(new_content)
                print(f"Updated: {full_path}")

if __name__ == "__main__":
    if len(sys.argv) != 3:
        print("Usage: python move.py <old_path> <new_path>")
        sys.exit(1)

    old_path_input = sys.argv[1]
    new_path_input = sys.argv[2]
    move_and_update(old_path_input, new_path_input)

