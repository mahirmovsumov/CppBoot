#include <gtest/gtest.h>

#include <atomic>
#include <cppboot/di/container.hpp>
#include <latch>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using cppboot::ErrorCode;
using cppboot::Exception;
using cppboot::di::Container;
using cppboot::di::Deps;
using cppboot::di::Resolver;
using cppboot::di::Scope;

namespace {

class Repository {
public:
    virtual ~Repository() = default;
    virtual std::string name() const = 0;
};

class SqlRepository : public Repository {
public:
    std::string name() const override { return "sql"; }
};

class Service {
public:
    explicit Service(std::shared_ptr<Repository> repository) : repository_(std::move(repository)) {}
    const std::shared_ptr<Repository>& repository() const { return repository_; }

private:
    std::shared_ptr<Repository> repository_;
};

class Plain {};

// Types for the cycle tests. Each takes the next by constructor.
class CycleB;
class CycleA {
public:
    explicit CycleA(std::shared_ptr<CycleB>) {}
};
class CycleB {
public:
    explicit CycleB(std::shared_ptr<CycleA>) {}
};

class ScopedThing {};
class TransientThing {
public:
    explicit TransientThing(std::shared_ptr<ScopedThing> scoped) : scoped_(std::move(scoped)) {}
    const std::shared_ptr<ScopedThing>& scoped() const { return scoped_; }

private:
    std::shared_ptr<ScopedThing> scoped_;
};
class HoldsTransient {
public:
    explicit HoldsTransient(std::shared_ptr<TransientThing>) {}
};
class HoldsScoped {
public:
    explicit HoldsScoped(std::shared_ptr<ScopedThing>) {}
};

// For the tests of dependencies taken by reference.
class Counted {
public:
    Counted() { ++alive; }
    Counted(const Counted&) = delete;
    Counted& operator=(const Counted&) = delete;
    ~Counted() { --alive; }
    int value() const { return 42; }

    static inline int alive = 0;
};

class Borrower {
public:
    explicit Borrower(Counted& counted) : counted_(counted) {}
    ~Borrower() { value_at_destruction = counted_.value(); }
    int value() const { return counted_.value(); }
    const Counted* address() const { return &counted_; }

    static inline int value_at_destruction = 0;

private:
    Counted& counted_;
};

class ConstBorrower {
public:
    explicit ConstBorrower(const Repository& repository) : repository_(repository) {}
    std::string name() const { return repository_.name(); }

private:
    const Repository& repository_;
};

class MixedOwner {
public:
    MixedOwner(Counted& counted, std::shared_ptr<const Repository> repository)
        : counted_(counted), repository_(std::move(repository)) {}
    std::string describe() const {
        return repository_->name() + std::to_string(counted_.value());
    }

private:
    Counted& counted_;
    std::shared_ptr<const Repository> repository_;
};

// Records its creation, to check the order dependencies are created in.
template <int Id>
class Ordered {
public:
    Ordered() { order().push_back(Id); }
    static std::vector<int>& order() {
        static std::vector<int> values;
        return values;
    }
};
using First = Ordered<1>;
using Second = Ordered<2>;
class TakesTwo {
public:
    TakesTwo(First&, Second&) {}
};

// Records its destruction, to check the order components are destroyed in.
class Tracked {
public:
    Tracked(std::vector<std::string>& log, std::string name)
        : log_(log), name_(std::move(name)) {}
    ~Tracked() { log_.push_back(name_); }

private:
    std::vector<std::string>& log_;
    std::string name_;
};

}  // namespace

TEST(ContainerTest, CreatesAComponentWithItsDependencies) {
    Container container;
    container.registerSingleton<Repository, SqlRepository>();
    container.registerSingleton<Service>(Deps<Repository>{});

    EXPECT_EQ(container.resolve<Service>()->repository()->name(), "sql");
}

TEST(ContainerReferenceTest, PassesADependencyByReference) {
    Container container;
    container.registerSingleton<Counted>();
    container.registerSingleton<Borrower>(Deps<Counted>{});

    const auto borrower = container.resolve<Borrower>();
    EXPECT_EQ(borrower->value(), 42);
    EXPECT_EQ(borrower->address(), container.resolve<Counted>().get());
}

TEST(ContainerReferenceTest, PassesAnInterfaceByConstReference) {
    Container container;
    container.registerSingleton<Repository, SqlRepository>();
    container.registerSingleton<ConstBorrower>(Deps<Repository>{});
    EXPECT_EQ(container.resolve<ConstBorrower>()->name(), "sql");
}

TEST(ContainerReferenceTest, MixesReferencesAndSharedPointers) {
    Container container;
    container.registerSingleton<Counted>();
    container.registerSingleton<Repository, SqlRepository>();
    container.registerSingleton<MixedOwner>(Deps<Counted, Repository>{});
    EXPECT_EQ(container.resolve<MixedOwner>()->describe(), "sql42");
}

TEST(ContainerReferenceTest, ATransientDependencyLivesAsLongAsWhatBorrowsIt) {
    Counted::alive = 0;
    Borrower::value_at_destruction = 0;
    {
        Container container;
        container.registerTransient<Counted>();
        container.registerSingleton<Borrower>(Deps<Counted>{});

        const auto borrower = container.resolve<Borrower>();

        // Nothing else holds this transient, which would be gone by now without the container.
        EXPECT_EQ(Counted::alive, 1);
        EXPECT_EQ(borrower->value(), 42);
    }
    EXPECT_EQ(Counted::alive, 0);
    EXPECT_EQ(Borrower::value_at_destruction, 42);
}

TEST(ContainerReferenceTest, ATransientBorrowerReleasesItsOwnDependency) {
    Counted::alive = 0;
    Container container;
    container.registerTransient<Counted>();
    container.registerTransient<Borrower>(Deps<Counted>{});

    {
        const auto borrower = container.resolve<Borrower>();
        EXPECT_EQ(Counted::alive, 1);
    }
    EXPECT_EQ(Counted::alive, 0);
}

TEST(ContainerReferenceTest, ABorrowerHeldAfterItsContainerIsGoneStaysSafe) {
    Counted::alive = 0;
    std::shared_ptr<Borrower> kept;
    {
        Container container;
        container.registerSingleton<Counted>();
        container.registerSingleton<Borrower>(Deps<Counted>{});
        kept = container.resolve<Borrower>();
    }
    EXPECT_EQ(Counted::alive, 1);
    EXPECT_EQ(kept->value(), 42);

    kept.reset();
    EXPECT_EQ(Counted::alive, 0);
}

TEST(ContainerReferenceTest, ABorrowerInAScopeUsesItsScopedDependencySafely) {
    Counted::alive = 0;
    Container container;
    container.registerScoped<Counted>();
    container.registerScoped<Borrower>(Deps<Counted>{});
    {
        Scope scope{container};
        EXPECT_EQ(scope.resolve<Borrower>()->value(), 42);
        EXPECT_EQ(Counted::alive, 1);
    }
    EXPECT_EQ(Counted::alive, 0);
}

TEST(ContainerReferenceTest, ValidateStillRejectsASingletonBorrowingAScopedComponent) {
    Container container;
    container.registerScoped<Counted>();
    container.registerSingleton<Borrower>(Deps<Counted>{});
    const auto valid = container.validate();
    ASSERT_FALSE(valid);
    EXPECT_NE(valid.error().message().find("scoped"), std::string::npos);
}

TEST(ContainerReferenceTest, DependenciesAreCreatedFromLeftToRight) {
    First::order().clear();
    Container container;
    container.registerSingleton<First>();
    container.registerSingleton<Second>();
    container.registerSingleton<TakesTwo>(Deps<First, Second>{});

    static_cast<void>(container.resolve<TakesTwo>());
    EXPECT_EQ(First::order(), (std::vector<int>{1}));
    EXPECT_EQ(Second::order(), (std::vector<int>{2}));
}

TEST(ContainerTest, SingletonIsCreatedOnce) {
    Container container;
    container.registerSingleton<Plain>();
    EXPECT_EQ(container.resolve<Plain>(), container.resolve<Plain>());
}

TEST(ContainerTest, TransientIsCreatedEachTime) {
    Container container;
    container.registerTransient<Plain>();
    EXPECT_NE(container.resolve<Plain>(), container.resolve<Plain>());
}

TEST(ContainerTest, ComponentsAreCreatedOnlyWhenNeeded) {
    int created = 0;
    Container container;
    container.registerSingleton<Plain>([&](Resolver&) {
        ++created;
        return std::make_shared<Plain>();
    });

    EXPECT_EQ(created, 0);
    static_cast<void>(container.resolve<Plain>());
    static_cast<void>(container.resolve<Plain>());
    EXPECT_EQ(created, 1);
}

TEST(ContainerTest, ExternalInstanceIsReturnedAsGiven) {
    const auto instance = std::make_shared<Plain>();
    Container container;
    container.registerInstance(instance);
    EXPECT_EQ(container.resolve<Plain>(), instance);
}

TEST(ContainerTest, NullExternalInstanceThrows) {
    Container container;
    EXPECT_THROW(container.registerInstance(std::shared_ptr<Plain>{}), Exception);
}

TEST(ContainerTest, FactoryCanResolveDependencies) {
    Container container;
    container.registerSingleton<Repository, SqlRepository>();
    container.registerTransient<Service>([](Resolver& resolver) {
        return std::make_shared<Service>(resolver.resolve<Repository>());
    });

    EXPECT_EQ(container.resolve<Service>()->repository()->name(), "sql");
}

TEST(ContainerTest, FactoryReturningNullThrows) {
    Container container;
    container.registerSingleton<Plain>([](Resolver&) { return std::shared_ptr<Plain>{}; });
    EXPECT_THROW(static_cast<void>(container.resolve<Plain>()), Exception);
}

TEST(ContainerTest, FailedSingletonCanBeRetried) {
    int attempts = 0;
    Container container;
    container.registerSingleton<Plain>([&](Resolver&) {
        if (++attempts == 1) {
            throw std::runtime_error("not yet");
        }
        return std::make_shared<Plain>();
    });

    EXPECT_THROW(static_cast<void>(container.resolve<Plain>()), std::runtime_error);
    EXPECT_NE(container.resolve<Plain>(), nullptr);
    EXPECT_EQ(attempts, 2);
}

TEST(ContainerTest, BindSharesTheInstanceOfTheComponent) {
    Container container;
    container.registerSingleton<SqlRepository>();
    container.bind<Repository, SqlRepository>();

    EXPECT_EQ(container.resolve<Repository>().get(), container.resolve<SqlRepository>().get());
}

TEST(ContainerTest, GroupReturnsEveryMemberInOrder) {
    class FirstRepository : public SqlRepository {};
    class SecondRepository : public SqlRepository {};

    Container container;
    container.registerSingleton<FirstRepository>();
    container.registerSingleton<SecondRepository>();
    container.bindAll<Repository, FirstRepository>();
    container.bindAll<Repository, SecondRepository>();

    const auto repositories = container.resolveAll<Repository>();
    ASSERT_EQ(repositories.size(), 2U);
    EXPECT_EQ(repositories[0].get(), container.resolve<FirstRepository>().get());
    EXPECT_EQ(repositories[1].get(), container.resolve<SecondRepository>().get());
    EXPECT_TRUE(container.validate());
}

TEST(ContainerTest, UnknownGroupIsEmpty) {
    Container container;
    EXPECT_TRUE(container.resolveAll<Repository>().empty());
}

TEST(ContainerTest, GroupMembersAreNotNormalBindings) {
    Container container;
    container.registerSingleton<SqlRepository>();
    container.bindAll<Repository, SqlRepository>();
    EXPECT_THROW(static_cast<void>(container.resolve<Repository>()), Exception);
}

TEST(ValidateTest, ReportsAGroupMemberThatIsNotRegistered) {
    Container container;
    container.bindAll<Repository, SqlRepository>();

    const auto result = container.validate();
    ASSERT_FALSE(result);
    EXPECT_NE(result.error().message().find("isn't registered"), std::string::npos);
}

// On MinGW, types with internal linkage sort inconsistently against other types, which once made
// registered types impossible to find.
TEST(ContainerTest, FindsTypesWhenLocalAndExternalTypesAreMixed) {
    struct LocalA {};
    struct LocalB {};
    struct LocalC {};

    Container container;
    container.registerSingleton<std::string>();
    container.registerSingleton<LocalA>();
    container.registerSingleton<std::vector<int>>();
    container.registerSingleton<LocalB>();
    container.registerSingleton<Plain>();
    container.registerSingleton<std::vector<char>>();
    container.registerSingleton<LocalC>();
    container.registerSingleton<std::vector<long>>();

    EXPECT_TRUE(container.contains(typeid(std::string)));
    EXPECT_TRUE(container.contains(typeid(LocalA)));
    EXPECT_TRUE(container.contains(typeid(std::vector<int>)));
    EXPECT_TRUE(container.contains(typeid(LocalB)));
    EXPECT_TRUE(container.contains(typeid(Plain)));
    EXPECT_TRUE(container.contains(typeid(std::vector<char>)));
    EXPECT_TRUE(container.contains(typeid(LocalC)));
    EXPECT_TRUE(container.contains(typeid(std::vector<long>)));
    EXPECT_NE(container.resolve<LocalB>(), nullptr);
    EXPECT_NE(container.resolve<std::vector<long>>(), nullptr);
}

TEST(ContainerTest, DuplicateRegistrationThrows) {
    Container container;
    container.registerSingleton<Plain>();

    try {
        container.registerTransient<Plain>();
        FAIL() << "Expected an exception";
    } catch (const Exception& exception) {
        EXPECT_EQ(exception.error().code(), ErrorCode::DependencyError);
        EXPECT_NE(exception.error().message().find("already registered"), std::string::npos);
    }
}

TEST(ContainerTest, MissingComponentNamesTheChain) {
    Container container;
    container.registerSingleton<Service>(Deps<Repository>{});

    try {
        static_cast<void>(container.resolve<Service>());
        FAIL() << "Expected an exception";
    } catch (const Exception& exception) {
        EXPECT_NE(exception.error().message().find("Service"), std::string::npos);
        EXPECT_NE(exception.error().message().find("Repository"), std::string::npos);
    }
}

TEST(ContainerTest, ContainsReportsRegistrations) {
    Container container;
    container.registerSingleton<Plain>();
    EXPECT_TRUE(container.contains(typeid(Plain)));
    EXPECT_FALSE(container.contains(typeid(Service)));
}

TEST(ContainerTest, ResolvingACycleThrowsInsteadOfOverflowingTheStack) {
    Container container;
    container.registerTransient<CycleA>(Deps<CycleB>{});
    container.registerTransient<CycleB>(Deps<CycleA>{});

    try {
        static_cast<void>(container.resolve<CycleA>());
        FAIL() << "Expected an exception";
    } catch (const Exception& exception) {
        EXPECT_NE(exception.error().message().find("Circular dependency"), std::string::npos);
    }
}

TEST(ContainerTest, ACycleInsideFactoriesIsDetected) {
    Container container;
    container.registerSingleton<CycleA>([](Resolver& resolver) {
        return std::make_shared<CycleA>(resolver.resolve<CycleB>());
    });
    container.registerSingleton<CycleB>([](Resolver& resolver) {
        return std::make_shared<CycleB>(resolver.resolve<CycleA>());
    });

    EXPECT_THROW(static_cast<void>(container.resolve<CycleA>()), Exception);
}

TEST(ContainerTest, ResolvingADiamondIsNotACycle) {
    struct Base {};
    struct Left {
        explicit Left(std::shared_ptr<Base>) {}
    };
    struct Right {
        explicit Right(std::shared_ptr<Base>) {}
    };
    struct Top {
        Top(std::shared_ptr<Left>, std::shared_ptr<Right>) {}
    };

    Container container;
    container.registerSingleton<Base>();
    container.registerTransient<Left>(Deps<Base>{});
    container.registerTransient<Right>(Deps<Base>{});
    container.registerTransient<Top>(Deps<Left, Right>{});

    EXPECT_NE(container.resolve<Top>(), nullptr);
    EXPECT_TRUE(container.validate());
}

TEST(ContainerTest, SingletonsAreDestroyedInReverseCreationOrder) {
    std::vector<std::string> log;
    struct First : Tracked {
        using Tracked::Tracked;
    };
    struct Second : Tracked {
        using Tracked::Tracked;
    };

    {
        Container container;
        container.registerSingleton<First>(
            [&](Resolver&) { return std::make_shared<First>(log, "first"); });
        container.registerSingleton<Second>(
            [&](Resolver&) { return std::make_shared<Second>(log, "second"); });

        static_cast<void>(container.resolve<First>());
        static_cast<void>(container.resolve<Second>());
    }

    EXPECT_EQ(log, (std::vector<std::string>{"second", "first"}));
}

TEST(ContainerTest, ConcurrentResolvesCreateOneSingleton) {
    constexpr int kThreads = 8;
    std::atomic<int> created{0};
    Container container;
    container.registerSingleton<Plain>([&](Resolver&) {
        ++created;
        return std::make_shared<Plain>();
    });

    std::latch start{kThreads};
    std::vector<std::shared_ptr<Plain>> results(kThreads);
    {
        std::vector<std::jthread> threads;
        for (int i = 0; i < kThreads; ++i) {
            threads.emplace_back([&, i] {
                start.arrive_and_wait();
                results[static_cast<std::size_t>(i)] = container.resolve<Plain>();
            });
        }
    }

    EXPECT_EQ(created.load(), 1);
    for (const auto& result : results) {
        EXPECT_EQ(result, results.front());
    }
}

TEST(ScopeTest, ScopedComponentIsSharedWithinAScope) {
    Container container;
    container.registerScoped<Plain>();

    Scope scope{container};
    EXPECT_EQ(scope.resolve<Plain>(), scope.resolve<Plain>());
}

TEST(ScopeTest, ScopesHaveSeparateInstances) {
    Container container;
    container.registerScoped<Plain>();

    Scope first{container};
    Scope second{container};
    EXPECT_NE(first.resolve<Plain>(), second.resolve<Plain>());
}

TEST(ScopeTest, ScopedComponentOutsideAScopeThrows) {
    Container container;
    container.registerScoped<Plain>();
    EXPECT_THROW(static_cast<void>(container.resolve<Plain>()), Exception);
}

TEST(ScopeTest, ScopeSeesSingletonsAndTransients) {
    Container container;
    container.registerSingleton<Repository, SqlRepository>();
    container.registerScoped<ScopedThing>();
    container.registerTransient<TransientThing>(Deps<ScopedThing>{});

    Scope scope{container};
    EXPECT_EQ(scope.resolve<Repository>(), container.resolve<Repository>());
    EXPECT_EQ(scope.resolve<TransientThing>()->scoped(), scope.resolve<ScopedThing>());
}

TEST(ScopeTest, SingletonCannotCaptureAScopedComponentAtRuntime) {
    Container container;
    container.registerScoped<ScopedThing>();
    container.registerSingleton<HoldsScoped>([](Resolver& resolver) {
        return std::make_shared<HoldsScoped>(resolver.resolve<ScopedThing>());
    });

    Scope scope{container};
    EXPECT_THROW(static_cast<void>(scope.resolve<HoldsScoped>()), Exception);
}

TEST(ScopeTest, ScopeStillWorksAfterASingletonFailsInsideIt) {
    Container container;
    container.registerScoped<ScopedThing>();
    container.registerSingleton<HoldsScoped>([](Resolver& resolver) {
        return std::make_shared<HoldsScoped>(resolver.resolve<ScopedThing>());
    });

    Scope scope{container};
    EXPECT_THROW(static_cast<void>(scope.resolve<HoldsScoped>()), Exception);
    EXPECT_NE(scope.resolve<ScopedThing>(), nullptr);
}

TEST(ScopeTest, ComponentsAreDestroyedWithTheScopeInReverseOrder) {
    std::vector<std::string> log;
    struct First : Tracked {
        using Tracked::Tracked;
    };
    struct Second : Tracked {
        using Tracked::Tracked;
    };

    Container container;
    container.registerScoped<First>(
        [&](Resolver&) { return std::make_shared<First>(log, "first"); });
    container.registerScoped<Second>(
        [&](Resolver&) { return std::make_shared<Second>(log, "second"); });

    {
        Scope scope{container};
        static_cast<void>(scope.resolve<First>());
        static_cast<void>(scope.resolve<Second>());
        EXPECT_TRUE(log.empty());
    }
    EXPECT_EQ(log, (std::vector<std::string>{"second", "first"}));
}

TEST(ValidateTest, AcceptsAValidGraphWithoutCreatingAnything) {
    int created = 0;
    Container container;
    container.registerSingleton<Repository, SqlRepository>();
    container.registerSingleton<Service>(Deps<Repository>{});
    container.registerTransient<Plain>([&](Resolver&) {
        ++created;
        return std::make_shared<Plain>();
    });

    EXPECT_TRUE(container.validate());
    EXPECT_EQ(created, 0);
}

TEST(ValidateTest, ReportsAMissingDependency) {
    Container container;
    container.registerSingleton<Service>(Deps<Repository>{});

    const auto result = container.validate();
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code(), ErrorCode::DependencyError);
    EXPECT_NE(result.error().message().find("isn't registered"), std::string::npos);
}

TEST(ValidateTest, ReportsACycle) {
    Container container;
    container.registerSingleton<CycleA>(Deps<CycleB>{});
    container.registerSingleton<CycleB>(Deps<CycleA>{});

    const auto result = container.validate();
    ASSERT_FALSE(result);
    EXPECT_NE(result.error().message().find("Circular dependency"), std::string::npos);
}

TEST(ValidateTest, ReportsASingletonDependingOnAScopedComponent) {
    Container container;
    container.registerScoped<ScopedThing>();
    container.registerSingleton<HoldsScoped>(Deps<ScopedThing>{});

    const auto result = container.validate();
    ASSERT_FALSE(result);
    EXPECT_NE(result.error().message().find("outlive"), std::string::npos);
}

TEST(ValidateTest, ReportsASingletonReachingAScopedComponentThroughATransient) {
    Container container;
    container.registerScoped<ScopedThing>();
    container.registerTransient<TransientThing>(Deps<ScopedThing>{});
    container.registerSingleton<HoldsTransient>(Deps<TransientThing>{});

    EXPECT_FALSE(container.validate());
}

TEST(ValidateTest, BindCountsAsADependencyOnTheComponent) {
    Container container;
    container.bind<Repository, SqlRepository>();
    EXPECT_FALSE(container.validate());

    container.registerSingleton<SqlRepository>();
    EXPECT_TRUE(container.validate());
}
