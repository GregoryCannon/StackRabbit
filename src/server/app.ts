import { PreComputeManager } from "./precompute";
import { RequestHandler } from "./request_handler";
import * as express from "express";

const port = 3000;
const ALLOW_MULTITHREAD = true;

function initExpressServer(requestHandler){
  const app = express();
  app.get("*", function (req: any, res: any) {
    // Log the request
    console.log("\n-------------------------\nlocalhost:" + port + req.url);
    console.time("Full request");

    // Main processing
    const [response, responseCode] = requestHandler.routeRequest(req);

    // Send response
    console.timeEnd("Full request");
    console.log("Sending response:", response, responseCode);
    res.setHeader("Access-Control-Allow-Origin", "*");
    res.setHeader("Content-Type", "text/plain");
    res.statusCode = responseCode;
    res.end(response);
  })

  app.listen(port);
  console.log("Listening on port", port);
}

if (ALLOW_MULTITHREAD){
  // Create an object to manage the worker threads involved in heavy placement computation
  const precomputer = new PreComputeManager();

  // Start the server once the precomputer is operational
  precomputer.initialize(() => {
    const requestHandler = new RequestHandler(precomputer);
    initExpressServer(requestHandler);
  });
} else {
  const requestHandler = new RequestHandler(null);
    initExpressServer(requestHandler);
}
