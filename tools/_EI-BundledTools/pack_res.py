import os
import platform
import subprocess

# Packed resources folder
res = "res"
# Unpacked resources folder
rext = "res_unpacked"

total_packed = ""

# Determine Wine executable path under Linux
wine_executable = "wine" if platform.system() == "Linux" else ""

# Change directory to the folder containing the unpacked resources
os.chdir(rext)

# Loop over *_res files
for filename in os.listdir("."):
    if filename.endswith("_res"):
        # Output filename
        out = filename.replace("_res", ".res")

        # Execute eipacker.exe with Wine under Linux
        if wine_executable:
            subprocess.run([wine_executable, "../bin/eipacker.exe", "/pack", filename])
        else:
            # Execute eipacker.exe directly under Windows
            subprocess.run(["../bin/eipacker.exe", "/pack", filename], shell=True)

        print(f"Done packing {filename} -> {out}")

        # Use rsync to move packed files to desired folder (under Linux)
        if platform.system() == "Linux":
            subprocess.run(["rsync", "-r", "--remove-source-files", f"./{out}", f"../{res}/"])
            print(f"Rsync completed on {out}")
        else:
            # Use robocopy to move packed files to desired folder (under Windows)
            subprocess.run(["robocopy", f"./{out}", f"../{res}/", "/MOVE"], shell=True)
            print(f"Robocopy completed on {out}")

        # Delete empty directories
        subprocess.run(["find", ".", "-depth", "-type", "d", "-empty", "-delete"])
        print("Empty directories deleted")

        # Add processed file to the list of processed files
        total_packed += f" {filename}"

print("DONE PACKING ASSETS")
print(f"Processed the following file(s): {total_packed}")
