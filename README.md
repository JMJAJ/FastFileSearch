# FastFileSearch

A high-performance file search utility for Windows with a modern UI, built using Dear ImGui and DirectX 11.

## Features

- Fast file search using KMP (Knuth-Morris-Pratt) algorithm
- Multi-threaded search for optimal performance
- Real-time search progress and timing information
- Support for regular expressions
- Case-sensitive/insensitive search options
- Modern, clean UI with DirectX 11 rendering
- Click-to-open file location in Explorer

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
6. Click on any result to open its location in File Explorer

## Performance

The application uses the KMP algorithm for string matching, providing:
- Best case time complexity: O(M)
- Worst case time complexity: O(M + N)
Where M is the length of the text and N is the length of the pattern.

## Requirements

- Windows
- DirectX 11 capable graphics card
- Visual Studio 2019 or later (for building)
