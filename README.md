# List and remove Logitech Bolt receiver pairings

macOS, receiver USB ID `046d:c548`.

## Build

```bash
clang -O1 -Wall -o bolt bolt.c -framework IOKit -framework CoreFoundation
```

## Usage

```bash
./bolt list                                        # list pairings
./bolt unpair 2                                    # remove slot 2
./bolt -v list                                     # also print HID++ reports
for s in 1 2 3 4 5 6; do ./bolt unpair $s; done    # remove all
```
