/*
 *  The contents of this file are subject to the Initial
 *  Developer's Public License Version 1.0 (the "License");
 *  you may not use this file except in compliance with the
 *  License. You may obtain a copy of the License at
 *  http://www.ibphoenix.com/main.nfs?a=ibphoenix&page=ibp_idpl.
 *
 *  Software distributed under the License is distributed AS IS,
 *  WITHOUT WARRANTY OF ANY KIND, either express or implied.
 *  See the License for the specific language governing rights
 *  and limitations under the License.
 *
 *  The Original Code was created by Dmitry Yemanov
 *  for the Firebird Open Source RDBMS project.
 *
 *  Copyright (c) 2009 Dmitry Yemanov <dimitr@firebirdsql.org>
 *  and all contributors signed below.
 *
 *  All Rights Reserved.
 *  Contributor(s): ______________________________________.
 */

#ifndef JRD_CURSOR_H
#define JRD_CURSOR_H

#include "../common/classes/array.h"
#include "../jrd/MetaName.h"

namespace Jrd
{
	class thread_db;
	class CompilerScratch;
	class RecordSource;
	class RseNode;

	// Select class (common base for sub-queries and cursors)

	class Select
	{
	public:
		enum : ULONG {
			SUB_QUERY = 1,
			INVARIANT = 2
		};

		Select(const RecordSource* source, const RseNode* rse,
			   ULONG line = 0, ULONG column = 0, const MetaName& cursorName = "")
			: m_top(source), m_rse(rse), m_cursorName(cursorName),
			  m_line(line), m_column(column)
		{}

		virtual ~Select()
		{}

		const RecordSource* getAccessPath() const
		{
			return m_top;
		}

		void initializeInvariants(Request* request) const;
		void printPlan(thread_db* tdbb, Firebird::string& plan, bool detailed) const;

		virtual void open(thread_db* tdbb) const = 0;
		virtual void close(thread_db* tdbb) const = 0;

	protected:
		const RecordSource* const m_top;
		const RseNode* const m_rse;
		MetaName m_cursorName;	// optional name for explicit PSQL cursors

	private:
		ULONG m_line = 0;
		ULONG m_column = 0;
	};

	// SubQuery class (simplified forward-only cursor)

	class SubQuery final : public Select
	{
	public:
		SubQuery(const RecordSource* rsb, const RseNode* rse);

		void open(thread_db* tdbb) const override;
		void close(thread_db* tdbb) const override;

		bool fetch(thread_db* tdbb) const;
	};

	// Cursor class (wrapper around the whole access tree)

	class Cursor final : public Select
	{
		enum State { BOS, POSITIONED, EOS };

		struct Impure
		{
			bool irsb_active;
			State irsb_state;
			FB_UINT64 irsb_position;
		};

	public:
		Cursor(CompilerScratch* csb, const RecordSource* rsb, const RseNode* rse,
			   bool updateCounters, ULONG line, ULONG column, const MetaName& name = "");

		void open(thread_db* tdbb) const override;
		void close(thread_db* tdbb) const override;

		bool fetchNext(thread_db* tdbb) const;
		bool fetchPrior(thread_db* tdbb) const;
		bool fetchFirst(thread_db* tdbb) const;
		bool fetchLast(thread_db* tdbb) const;
		bool fetchAbsolute(thread_db* tdbb, SINT64 offset) const;
		bool fetchRelative(thread_db* tdbb, SINT64 offset) const;

		void checkState(Request* request) const;

#if (!defined __GNUC__) || (__GNUC__ > 6)
		constexpr
#endif
				  bool isUpdateCounters() const
		{
			return m_updateCounters;
		}

		const MetaName& getName() const
		{
			return m_cursorName;
		}

	private:
		ULONG m_impure;
		const bool m_updateCounters;
	};

} // namespace

#endif // JRD_CURSOR_H
