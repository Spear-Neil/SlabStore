import re
import sys


def extract_total_access(filename, field_name="TotalMediaWrites"):
    total = 0
    with open(filename, 'r') as f:
        for line in f:
            if field_name in line:
                match = re.search(rf'{field_name}=([0-9a-fx]+)', line, re.IGNORECASE)
                if match:
                    hex_str = match.group(1).lower().replace('0x', '').lstrip('0')
                    if hex_str:
                        total += int(hex_str, 16)
    return total


def main():
    if len(sys.argv) < 3:
        print("usage: python3 script.py before.txt after.txt [field-name]")
        print("default field: TotalMediaWrites")
        return

    before_file, after_file = sys.argv[1], sys.argv[2]
    field_name = sys.argv[3] if len(sys.argv) > 3 else "TotalMediaWrites"

    before_total = extract_total_access(before_file, field_name)
    after_total = extract_total_access(after_file, field_name)
    difference = after_total - before_total

    print(f"{field_name} (bytes): {difference * 64}")


if __name__ == "__main__":
    main()
