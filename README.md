# FastFileSearch (Beta)

A high-performance file search utility for Windows with a modern UI, built using Dear ImGui and DirectX 11.
(Still learning about searching algos and other methods) ![lin](https://media.tenor.com/0z7sLx2ohBkAAAAi/tower-of-fantasy-tof.gif))

## Features

- Fast file search using KMP (Knuth-Morris-Pratt) algorithm
- Multi-threaded search for optimal performance
- Real-time search progress and timing information
- Support for regular expressions
- Case-sensitive/insensitive search options
- Modern, clean UI with DirectX 11 rendering
- Tree view display of search results
  - Hierarchical directory structure
  - Expandable/collapsible folders
  - Right-click context menus
  - File size and last modified date
- Interactive file operations:
  - Single click: Select file in Explorer
  - Double click: Open file directly
  - Right-click menu options:
    - Open file
    - Open containing folder
    - Copy file path
    - Expand/Collapse all (for directories)
- Persistent tree state between searches

## Building

### Prerequisites

- Windows
- Visual Studio 2022 (any edition)
- DirectX 11 capable graphics card

### Option 1: Using Visual Studio 2022 (Recommended)

1. Clone the repository with submodules:
```bash
git clone --recursive https://github.com/JMJAJ/FastFileSearch.git
# Or if you already cloned without submodules:
git submodule update --init --recursive
```

2. Open `FastSearch_Windows.sln` in Visual Studio 2022
3. Select your desired configuration (Debug/Release) and platform (x64/x86)
4. Build and run the project

### Option 2: Using MSBuild Command Line

1. Clone the repository with submodules as shown above
2. Open "Developer Command Prompt for VS 2022"
3. Navigate to the project directory:
```cmd
cd path\to\FastFileSearch
```
4. Build using MSBuild:
```cmd
msbuild FastSearch_Windows\FastSearch_Windows.vcxproj /p:Configuration=Release /p:Platform=x64
```

### Option 3: Using CMake

1. Clone the repository with submodules as shown above
2. Create and navigate to a build directory:
```bash
mkdir build
cd build
```
3. Configure and build:
```bash
cmake ..
cmake --build . --config Release
```

## Usage

1. Launch the application
2. Enter your search pattern
3. Select the folder to search in using the "Browse" button
4. Choose search options (case sensitivity, regex)
5. Click "Search" to begin
6. Navigate results using the tree view:
   - Click arrows or double-click to expand/collapse folders
   - Right-click for additional options
   - Use "Expand All" or "Collapse All" to quickly navigate large result sets
   - View file sizes and last modified dates in the table view

## Performance

The application uses the KMP algorithm for string matching, providing:
- Best case time complexity: O(M)
- Worst case time complexity: O(M + N)
Where M is the length of the text and N is the length of the pattern.

Multi-threaded search implementation ensures optimal performance on modern multi-core processors.

## Dependencies

All dependencies are included as Git submodules:
- [Dear ImGui](https://github.com/ocornut/imgui) - Immediate mode GUI library
- DirectX 11 (included with Windows SDK)

## Contributing

Contributions are welcome! Please feel free to submit pull requests with new features or bug fixes.

## License

This project is open source and available under the MIT License.
