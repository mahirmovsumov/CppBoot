#include <gtest/gtest.h>

#include <cppboot/web/router.hpp>
#include <stdexcept>
#include <string>
#include <vector>

using namespace cppboot::web;
using cppboot::Exception;

namespace {

Request makeRequest(Method method, std::string_view target) {
    return Request::create(method, target).value();
}

Handler replyWith(std::string body) {
    return [body = std::move(body)](const Request&) { return Response::text(body); };
}

}  // namespace

TEST(RequestTest, ReadsPathAndQuery) {
    const Request request = makeRequest(Method::Get, "/users/7?page=2&name=a%20b&plus=c+d&flag");
    EXPECT_EQ(request.path(), "/users/7");
    EXPECT_EQ(request.query("page"), "2");
    EXPECT_EQ(request.query("name"), "a b");
    EXPECT_EQ(request.query("plus"), "c d");
    EXPECT_EQ(request.query("flag"), "");
    EXPECT_FALSE(request.query("absent"));
}

TEST(RequestTest, ReadsHeadersIgnoringCase) {
    const Request request =
        Request::create(Method::Get, "/", Headers{{"Content-Type", "text/plain"}}).value();
    EXPECT_EQ(request.header("content-type"), "text/plain");
    EXPECT_FALSE(request.header("accept"));
}

TEST(RequestTest, RejectsInvalidTargets) {
    for (const char* target : {"", "users", "/a%2", "/a%", "/a%zz", "/a%00", "/?x=%0", "/?%00=1"}) {
        EXPECT_FALSE(Request::create(Method::Get, target)) << target;
    }
}

TEST(ResponseTest, SetHeaderReplacesIgnoringCase) {
    Response response;
    response.setHeader("X-Test", "one").setHeader("x-test", "two");
    EXPECT_EQ(response.headers().size(), 1U);
    EXPECT_EQ(response.header("X-TEST"), "two");
}

TEST(ResponseTest, RejectsHeaderInjection) {
    Response response;
    EXPECT_THROW(response.setHeader("X-Test", "a\r\nSet-Cookie: evil=1"), Exception);
    EXPECT_THROW(response.setHeader("X\nTest", "a"), Exception);
    EXPECT_THROW(response.setHeader("", "a"), Exception);
}

TEST(ResponseTest, ErrorResponseEscapesItsMessage) {
    const Response response = errorResponse(400, "bad \"value\"");
    EXPECT_EQ(response.status(), 400);
    EXPECT_EQ(response.body(), R"({"error":"bad \"value\"","status":400})");
    EXPECT_EQ(response.header("Content-Type"), "application/json");
}

TEST(RouterTest, MatchesMethodAndPath) {
    Router router;
    router.get("/users", replyWith("list")).post("/users", replyWith("create"));

    EXPECT_EQ(router.handle(makeRequest(Method::Get, "/users")).body(), "list");
    EXPECT_EQ(router.handle(makeRequest(Method::Post, "/users")).body(), "create");
}

TEST(RouterTest, FillsPathParameters) {
    Router router;
    router.get("/users/{id}/posts/{post}", [](const Request& request) {
        return Response::text(*request.param("id") + "," + *request.param("post"));
    });

    EXPECT_EQ(router.handle(makeRequest(Method::Get, "/users/7/posts/a%20b")).body(), "7,a b");
}

TEST(RouterTest, EncodedSlashStaysInsideItsSegment) {
    Router router;
    router.get("/files/{name}",
               [](const Request& request) { return Response::text(*request.param("name")); });

    const Response response = router.handle(makeRequest(Method::Get, "/files/a%2Fb"));
    EXPECT_EQ(response.status(), 200);
    EXPECT_EQ(response.body(), "a/b");
}

TEST(RouterTest, LiteralSegmentsBeatParameters) {
    Router router;
    router.get("/users/{id}", replyWith("param"));
    router.get("/users/me", replyWith("literal"));
    router.get("/{a}/me", replyWith("late"));

    EXPECT_EQ(router.handle(makeRequest(Method::Get, "/users/me")).body(), "literal");
    EXPECT_EQ(router.handle(makeRequest(Method::Get, "/users/7")).body(), "param");
    EXPECT_EQ(router.handle(makeRequest(Method::Get, "/other/me")).body(), "late");
}

TEST(RouterTest, IgnoresTrailingSlash) {
    Router router;
    router.get("/users", replyWith("list"));
    EXPECT_EQ(router.handle(makeRequest(Method::Get, "/users/")).status(), 200);
}

TEST(RouterTest, RootPathMatches) {
    Router router;
    router.get("/", replyWith("root"));
    EXPECT_EQ(router.handle(makeRequest(Method::Get, "/")).body(), "root");
}

TEST(RouterTest, UnknownPathIsNotFound) {
    Router router;
    router.get("/users", replyWith("list"));

    const Response response = router.handle(makeRequest(Method::Get, "/absent"));
    EXPECT_EQ(response.status(), 404);
    EXPECT_NE(response.body().find("Not Found"), std::string::npos);
}

TEST(RouterTest, WrongMethodReportsAllowedMethods) {
    Router router;
    router.get("/users", replyWith("list")).post("/users", replyWith("create"));

    const Response response = router.handle(makeRequest(Method::Delete, "/users"));
    EXPECT_EQ(response.status(), 405);
    EXPECT_EQ(response.header("Allow"), "GET, HEAD, POST");
}

TEST(RouterTest, HeadUsesTheGetRoute) {
    Router router;
    router.get("/users", replyWith("list"));
    EXPECT_EQ(router.handle(makeRequest(Method::Head, "/users")).status(), 200);
}

TEST(RouterTest, RejectsInvalidPatterns) {
    Router router;
    EXPECT_THROW(router.get("users", replyWith("")), Exception);
    EXPECT_THROW(router.get("", replyWith("")), Exception);
    EXPECT_THROW(router.get("/a/{id", replyWith("")), Exception);
    EXPECT_THROW(router.get("/a/id}", replyWith("")), Exception);
    EXPECT_THROW(router.get("/a/{}", replyWith("")), Exception);
    EXPECT_THROW(router.get("/{a}/{a}", replyWith("")), Exception);
    EXPECT_THROW(router.get("/a", Handler{}), Exception);
}

TEST(RouterTest, RejectsDuplicateRoutes) {
    Router router;
    router.get("/users/{id}", replyWith(""));
    EXPECT_THROW(router.get("/users/{other}", replyWith("")), Exception);
    EXPECT_NO_THROW(router.post("/users/{id}", replyWith("")));
    EXPECT_NO_THROW(router.get("/users/me", replyWith("")));
}

TEST(RouterTest, HttpErrorBecomesItsResponse) {
    Router router;
    router.get("/", [](const Request&) -> Response { throw HttpError(422, "Bad name"); });

    const Response response = router.handle(makeRequest(Method::Get, "/"));
    EXPECT_EQ(response.status(), 422);
    EXPECT_NE(response.body().find("Bad name"), std::string::npos);
}

TEST(RouterTest, UnexpectedExceptionsDoNotReachTheClient) {
    std::string logged;
    Router router;
    router.onUnhandledException([&](std::string_view what) { logged = what; });
    router.get("/", [](const Request&) -> Response {
        throw std::runtime_error("password=hunter2");
    });

    const Response response = router.handle(makeRequest(Method::Get, "/"));
    EXPECT_EQ(response.status(), 500);
    EXPECT_EQ(response.body().find("hunter2"), std::string::npos);
    EXPECT_EQ(logged, "password=hunter2");
}

TEST(RouterTest, MiddlewareRunsOutermostFirst) {
    std::vector<std::string> events;
    const auto record = [&](std::string name) {
        return [&events, name = std::move(name)](const Request& request, const Next& next) {
            events.push_back("before " + name);
            Response response = next(request);
            events.push_back("after " + name);
            return response;
        };
    };

    Router router;
    router.use(record("a")).use(record("b"));
    router.get("/", [&](const Request&) {
        events.emplace_back("handler");
        return Response{};
    });

    static_cast<void>(router.handle(makeRequest(Method::Get, "/")));
    EXPECT_EQ(events, (std::vector<std::string>{"before a", "before b", "handler", "after b",
                                                "after a"}));
}

TEST(RouterTest, MiddlewareCanStopARequest) {
    bool handler_ran = false;
    Router router;
    router.use([](const Request&, const Next&) { return errorResponse(401, "Sign in"); });
    router.get("/", [&](const Request&) {
        handler_ran = true;
        return Response{};
    });

    EXPECT_EQ(router.handle(makeRequest(Method::Get, "/")).status(), 401);
    EXPECT_FALSE(handler_ran);
}

TEST(RouterTest, MiddlewareCanChangeTheResponse) {
    Router router;
    router.use([](const Request& request, const Next& next) {
        Response response = next(request);
        response.setHeader("X-Added", "yes");
        return response;
    });
    router.get("/", replyWith("ok"));

    EXPECT_EQ(router.handle(makeRequest(Method::Get, "/")).header("X-Added"), "yes");
}

TEST(RouterTest, MiddlewareSeesParametersNotFoundAndFailures) {
    std::vector<std::string> seen;
    Router router;
    router.use([&](const Request& request, const Next& next) {
        Response response = next(request);
        seen.push_back(std::to_string(response.status()) + ":" +
                       request.param("id").value_or("-"));
        return response;
    });
    router.get("/users/{id}", [](const Request&) -> Response { throw HttpError(409, "Taken"); });

    static_cast<void>(router.handle(makeRequest(Method::Get, "/users/7")));
    static_cast<void>(router.handle(makeRequest(Method::Get, "/absent")));
    EXPECT_EQ(seen, (std::vector<std::string>{"409:7", "404:-"}));
}

TEST(RouterTest, RequestKnowsItsRoutePattern) {
    std::string seen;
    Router router;
    router.use([&](const Request& request, const Next& next) {
        seen = request.route();
        return next(request);
    });
    router.get("/users/{id}", replyWith("ok"));

    static_cast<void>(router.handle(makeRequest(Method::Get, "/users/7")));
    EXPECT_EQ(seen, "/users/{id}");

    static_cast<void>(router.handle(makeRequest(Method::Get, "/absent")));
    EXPECT_EQ(seen, "");
}

TEST(RouterTest, ExceptionInMiddlewareIsContained) {
    Router router;
    router.use([](const Request&, const Next&) -> Response { throw std::runtime_error("boom"); });
    EXPECT_EQ(router.handle(makeRequest(Method::Get, "/")).status(), 500);
}
