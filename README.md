Overview:
  ProxyCacheServer is a high-performance proxy server with integrated caching, designed to optimize web requests by reducing latency and improving data retrieval efficiency. The server currently supports GET requests, parsing them using an existing library, and caches responses for faster subsequent access.

Features:
  1.High Capacity: Supports up to 400 concurrent client requests.
  2.Integrated Caching: Stores frequently accessed data to minimize redundant requests.
  3.Error Handling: Sends appropriate HTTP error responses (e.g., 400, 403, 404, 500).
  4.Thread Management: Efficiently handles multiple clients using threads and semaphores.
  5.LRU Cache: Implements a Least Recently Used (LRU) cache replacement policy.

Configuration:
  Port Number: Default is 8080 (can be set via command line).
  Cache Size: Maximum cache size is 200MB, with a maximum element size of 10MB.

Usage:
  1. Compile the Code:
    gcc -o proxy_server proxy_server.c -lpthread
  2. Run the Server:
    ./proxy_server [port_number]
    NOTE : Replace [port_number] with your desired port number (default is 8080).

Note:
  Currently, only GET requests are supported. I'm working on other methods as well.

License:
  This project is licensed under the MIT License.
