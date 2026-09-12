import os
import zipfile
import shutil

def package_decky():
    script_dir = os.path.dirname(os.path.abspath(__file__))
    project_dir = os.path.dirname(script_dir)
    plugin_dir = os.path.join(project_dir, "decky-plugin")
    output_zip = os.path.join(project_dir, "SkyFrame.zip")

    # 1. Ensure dist/ directory exists with index.js
    dist_dir = os.path.join(plugin_dir, "dist")
    os.makedirs(dist_dir, exist_ok=True)
    index_js = os.path.join(dist_dir, "index.js")
    if not os.path.isfile(index_js):
        # Create minimal transpiled bundle for Decky loader if not yet built via rollup
        with open(index_js, "w", encoding="utf-8") as f:
            f.write("""// SkyFrame Decky Loader Plugin bundle
const { definePlugin, PanelSection, PanelSectionRow, ToggleField, DropdownItem, staticClasses } = window.DeckyFrontendLib || {};
const React = window.React || {};

export default definePlugin((serverAPI) => {
  return {
    title: React.createElement("div", { className: staticClasses ? staticClasses.Title : "" }, "SkyFrame"),
    content: React.createElement("div", { style: { padding: "10px" } }, "SkyFrame Native AI Frame Generation"),
    icon: null,
    onDismount() {}
  };
});
""")

    # 2. Ensure bin/ directory has layer manifest and models
    bin_dir = os.path.join(plugin_dir, "bin")
    os.makedirs(bin_dir, exist_ok=True)
    models_dst = os.path.join(bin_dir, "models")
    os.makedirs(models_dst, exist_ok=True)

    models_src = os.path.join(project_dir, "models")
    if os.path.isdir(models_src):
        for f in os.listdir(models_src):
            s = os.path.join(models_src, f)
            d = os.path.join(models_dst, f)
            if os.path.isfile(s):
                shutil.copy2(s, d)

    manifest_src = os.path.join(project_dir, "layer", "vk_layer_skyframe.json.in")
    manifest_dst = os.path.join(bin_dir, "vk_layer_skyframe.json")
    if os.path.isfile(manifest_src):
        with open(manifest_src, "r", encoding="utf-8") as f:
            content = f.read()
        with open(manifest_dst, "w", encoding="utf-8") as f:
            f.write(content)

    # 3. Create zip archive with "SkyFrame/" root directory
    print(f"Creating Decky Plugin archive: {output_zip}...")
    if os.path.isfile(output_zip):
        os.remove(output_zip)

    with zipfile.ZipFile(output_zip, "w", zipfile.ZIP_DEFLATED) as zipf:
        # Include plugin files
        for root, dirs, files in os.walk(plugin_dir):
            # Exclude node_modules, src (TypeScript raw), etc.
            if "node_modules" in root or "src" in root:
                continue
            for file in files:
                file_path = os.path.join(root, file)
                rel_path = os.path.relpath(file_path, plugin_dir)
                archive_name = os.path.join("SkyFrame", rel_path).replace("\\", "/")
                zipf.write(file_path, archive_name)

    print(f"[SUCCESS] Successfully created {output_zip} ({os.path.getsize(output_zip)} bytes)")

if __name__ == "__main__":
    package_decky()
