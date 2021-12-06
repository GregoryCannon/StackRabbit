import { PreComputeManager } from "./precompute";
import { RequestHandler } from "./request_handler";
import * as express from "express";

const IS_DEPLOY = process.env.DEPLOY;
const TETRIS_TRAINER_URL = "https://gregorycannon.github.io";

const port = process.env.PORT || 3000;
const ALLOW_MULTITHREAD = !IS_DEPLOY;

function initExpressServer(requestHandler) {
  const app = express();

  // Set CORS policy
  app.use((req, res, next) => {
    res.setHeader(
      "Access-Control-Allow-Origin",
      IS_DEPLOY ? TETRIS_TRAINER_URL : "*"
    );
    next();
  });

  app.get("*", function (req: any, res: any) {
    // Log the request
    console.log("\n-------------------------\nlocalhost:" + port + req.url);
    console.time("Full request");

    // Main processing
    const [response, responseCode] = requestHandler.routeRequest(req);

    // Send response
    console.timeEnd("Full request");
    // console.log("Sending response:", response, responseCode);
    res.setHeader("Content-Type", "text/plain");
    res.statusCode = responseCode;
    res.end(response);
  });

  app.listen(port);
  console.log("Listening on port", port);
}

if (ALLOW_MULTITHREAD) {
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
