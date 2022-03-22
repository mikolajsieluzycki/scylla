git pull
./configure.py --mode=dev --cflags="-g -ggdb"
ninja -t compdb > compile_commands.json
python clean_compile_commands.py
ninja dev-build
