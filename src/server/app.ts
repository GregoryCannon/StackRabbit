import { PreComputeManager } from "./precompute";
import { RequestHandler } from "./request_handler";
import * as http from "http";

const hostname = "127.0.0.1";
const port = 8080;

console.log("Starting server...");

/**
 * Create HTTP server for the frontend to request
 */
function initServer(requestHandler) {
  const server = http.createServer((req, res) => {
    res.statusCode = 200;
    res.setHeader("Access-Control-Allow-Origin", "*");
    res.setHeader("Content-Type", "text/plain");

    console.log("\n-------------------------\nlocalhost:" + port + req.url);
    console.time("Full request");

    const [response, responseCode] = requestHandler.routeRequest(req);

    console.timeEnd("Full request");
    console.log("Sending response:", response, responseCode);
    res.statusCode = responseCode;
    res.end(response);
  });

  server.listen(port, hostname, () => {
    console.log(`Server running at http://${hostname}:${port}/`);
  });
}

// Create an object to manage the worker threads involved in heavy placement computation
const precomputer = new PreComputeManager();

// Start the server once the precomputer is operational
precomputer.initialize(() => {
  const requestHandler = new RequestHandler(precomputer);
  initServer(requestHandler);
});
