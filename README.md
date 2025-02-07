# FastFileSearch

A high-performance file search utility for Windows with a modern UI, built using Dear ImGui and DirectX 11.

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

1. Make sure you have CMake and Visual Studio installed
2. Clone the repository
3. Run these commands:
```bash
mkdir build
cd build
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

## Requirements

- Windows
- DirectX 11 capable graphics card
- Visual Studio 2019 or later (for building)
- CMake 3.15 or later

## Contributing

Contributions are welcome! Please feel free to submit pull requests with new features or bug fixes.

## License

This project is open source and available under the MIT License.
