#pragma once

#include <cppboot/base/annotations.hpp>
#include <cppboot/data/database.hpp>
#include <cppboot/web/binding.hpp>
#include <cppboot/web/http.hpp>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace store {

struct CPPBOOT_JSON CPPBOOT_ROW Product {
    std::int64_t id{0};
    std::string name;
    std::optional<std::string> note;
    double price{0};
    bool in_stock{false};
};

struct CPPBOOT_JSON NewProduct {
    std::string name;
    std::optional<std::string> note;
    double price{0};
};

// Mapping rows inline in a header has to work, because that's where repositories usually go.
class CPPBOOT_SERVICE ProductRepository {
public:
    explicit ProductRepository(std::shared_ptr<cppboot::data::Database> database)
        : database_(std::move(database)) {}

    Product add(const NewProduct& details) {
        return *database_->queryOne<Product>(
            "INSERT INTO products (name, note, price) VALUES ($1, $2, $3) RETURNING *",
            details.name, details.note, details.price);
    }

    std::optional<Product> find(std::int64_t id) {
        return database_->queryOne<Product>("SELECT * FROM products WHERE id = $1", id);
    }

    std::vector<Product> all() {
        return database_->queryAs<Product>("SELECT * FROM products ORDER BY id");
    }

    // Adds several products as one unit, so that a failure leaves none of them.
    std::int64_t addAll(const std::vector<NewProduct>& products) {
        return database_->transaction([&](cppboot::data::Transaction& transaction) {
            std::int64_t added = 0;
            for (const NewProduct& details : products) {
                added += transaction.execute(
                    "INSERT INTO products (name, note, price) VALUES ($1, $2, $3)", details.name,
                    details.note, details.price);
            }
            return added;
        });
    }

private:
    std::shared_ptr<cppboot::data::Database> database_;
};

class CPPBOOT_CONTROLLER("/products") ProductController {
public:
    explicit ProductController(std::shared_ptr<ProductRepository> products)
        : products_(std::move(products)) {}

    CPPBOOT_GET("/") std::vector<Product> list() { return products_->all(); }

    CPPBOOT_GET("/{id}") Product get(std::int64_t id) {
        auto product = products_->find(id);
        if (!product) {
            throw cppboot::web::HttpError(404, "No such product");
        }
        return *product;
    }

    CPPBOOT_POST("/") cppboot::web::WithStatus<Product> create(const NewProduct& details) {
        return {201, products_->add(details)};
    }

    CPPBOOT_POST("/batch") std::int64_t createAll(const std::vector<NewProduct>& products) {
        return products_->addAll(products);
    }

private:
    std::shared_ptr<ProductRepository> products_;
};

}  // namespace store
