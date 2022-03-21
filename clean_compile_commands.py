with open("compile_commands.json", "r") as f:
    import json
    data = json.load(f)

filtered_data = [datum for datum in data if datum["file"].endswith("cc")]
with open("compile_commands.json", "w") as f:
    json.dump(filtered_data, f)
