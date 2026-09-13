import os
import zipfile
import shutil

def package_decky():
    script_dir = os.path.dirname(os.path.abspath(__file__))
    project_dir = os.path.dirname(script_dir)
    plugin_dir = os.path.join(project_dir, "decky-plugin")
    output_zip = os.path.join(project_dir, "SkyFrame.zip")

    # 1. Verify dist/ directory exists with compiled index.js
    dist_dir = os.path.join(plugin_dir, "dist")
    index_js = os.path.join(dist_dir, "index.js")
    if not os.path.isfile(index_js):
        raise RuntimeError("dist/index.js is missing! Run 'npm run build' inside decky-plugin first.")

    # 2. Ensure bin/ directory exists and report readiness
    # (CI/.github/workflows/build.yml and scripts/build_linux.sh stage
    # liblsfg-vk-layer.so, liblsfg-vk-layer_32.so and lsfg-vk-cli here.
    # Missing libs = installable UI-only zip: plugin will show 64/32 ✗.)
    bin_dir = os.path.join(plugin_dir, "bin")
    os.makedirs(bin_dir, exist_ok=True)
    expected = ["liblsfg-vk-layer.so", "liblsfg-vk-layer_32.so", "lsfg-vk-cli"]
    missing = [n for n in expected if not os.path.isfile(os.path.join(bin_dir, n))]
    staged = sorted(os.listdir(bin_dir))
    print(f"bin/ contents: {staged if staged else '<empty>'}")
    if missing:
        print(f"[WARN] Missing layer binaries in bin/: {missing} — zip will be UI-only.")

    # 3. Create zip archive with "SkyFrame/" root directory
    print(f"Creating Decky Plugin archive: {output_zip}...")
    if os.path.isfile(output_zip):
        os.remove(output_zip)

    with zipfile.ZipFile(output_zip, "w", zipfile.ZIP_DEFLATED) as zipf:
        # Include plugin files
        for root, dirs, files in os.walk(plugin_dir):
            # Exclude node_modules, src (TypeScript raw), etc.
            if "node_modules" in root or "__pycache__" in root or "src" in root:
                continue
            for file in files:
                if file.endswith((".pyc", ".pyo")):
                    continue
                file_path = os.path.join(root, file)
                rel_path = os.path.relpath(file_path, plugin_dir)
                archive_name = os.path.join("SkyFrame", rel_path).replace("\\", "/")
                zipf.write(file_path, archive_name)

    print(f"[SUCCESS] Successfully created {output_zip} ({os.path.getsize(output_zip)} bytes)")

if __name__ == "__main__":
    package_decky()
