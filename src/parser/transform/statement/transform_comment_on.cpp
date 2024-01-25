#include "duckdb/parser/expression/constant_expression.hpp"
#include "duckdb/parser/parsed_data/alter_info.hpp"
#include "duckdb/parser/parsed_data/alter_table_info.hpp"
#include "duckdb/parser/statement/alter_statement.hpp"
#include "duckdb/parser/transformer.hpp"

namespace duckdb {

unique_ptr<AlterStatement> Transformer::TransformCommentOn(duckdb_libpgquery::PGCommentOnStmt &stmt) {
	QualifiedName qualified_name;
	string column_name;

	if (stmt.object_type != duckdb_libpgquery::PG_OBJECT_COLUMN) {
		qualified_name = TransformQualifiedName(*stmt.name);
	} else {
		auto transformed_expr = TransformExpression(stmt.column_expr);

		if (transformed_expr->GetExpressionType() != ExpressionType::COLUMN_REF) {
			throw ParserException("Unexpected expression found, expected column reference to comment on (e.g. 'schema.table.column'), found '%s'", transformed_expr->ToString());
		}

		auto colref_expr = transformed_expr->Cast<ColumnRefExpression>();

		column_name = colref_expr.GetColumnName();
		qualified_name.name = colref_expr.column_names.size() > 1 ? colref_expr.GetTableName() : "";
		qualified_name.schema = colref_expr.column_names.size() > 2 ? colref_expr.GetSchemaName() : "";
		qualified_name.catalog = colref_expr.column_names.size() > 3 ? colref_expr.GetCatalogName() : "";

		if (colref_expr.column_names.size() < 2 || qualified_name.name.empty()) {
			throw ParserException("Specifying a table is required. eg. COMMENT ON COLUMN table1.col1 IS 'comment value'");
		}
	}

	auto res = make_uniq<AlterStatement>();
	unique_ptr<AlterInfo> info;

	auto expr = TransformExpression(stmt.value);
	if (expr->expression_class != ExpressionClass::CONSTANT) {
		throw NotImplementedException("Can only use constants as comments");
	}
	auto comment_value = expr->Cast<ConstantExpression>().value;

	CatalogType type = CatalogType::INVALID;

	// Regular CatalogTypes
	switch (stmt.object_type) {
	case duckdb_libpgquery::PG_OBJECT_TABLE:
		type = CatalogType::TABLE_ENTRY;
		break;
	case duckdb_libpgquery::PG_OBJECT_SCHEMA:
		type = CatalogType::SCHEMA_ENTRY;
		break;
	case duckdb_libpgquery::PG_OBJECT_INDEX:
		type = CatalogType::INDEX_ENTRY;
		break;
	case duckdb_libpgquery::PG_OBJECT_VIEW:
		type = CatalogType::VIEW_ENTRY;
		break;
	case duckdb_libpgquery::PG_OBJECT_FUNCTION:
		type = CatalogType::MACRO_ENTRY;
		break;
	case duckdb_libpgquery::PG_OBJECT_TABLE_MACRO:
		type = CatalogType::TABLE_MACRO_ENTRY;
		break;
	case duckdb_libpgquery::PG_OBJECT_SEQUENCE:
		type = CatalogType::SEQUENCE_ENTRY;
		break;
	case duckdb_libpgquery::PG_OBJECT_TYPE:
		type = CatalogType::TYPE_ENTRY;
		break;
	default:
		break;
	}

	if (type != CatalogType::INVALID) {
		info = make_uniq<SetCommentInfo>(type, qualified_name.catalog, qualified_name.schema,
		                                 qualified_name.name, comment_value, OnEntryNotFound::THROW_EXCEPTION);
	} else if (stmt.object_type == duckdb_libpgquery::PG_OBJECT_COLUMN) {


		// Special case: Table Column
		AlterEntryData alter_entry_data;
		alter_entry_data.catalog = qualified_name.catalog;
		alter_entry_data.schema = qualified_name.schema;
		alter_entry_data.name = qualified_name.name;
		alter_entry_data.if_not_found = OnEntryNotFound::THROW_EXCEPTION;

 		info = make_uniq<AlterColumnCommentInfo>(alter_entry_data, column_name, comment_value);
	} else if (stmt.object_type == duckdb_libpgquery::PG_OBJECT_DATABASE) {
		throw NotImplementedException("Adding comments to databases in not implemented");
	}

	if (info) {
		res->info = std::move(info);
		return res;
	}

	throw NotImplementedException("Can not comment on this type");
}

} // namespace duckdb
