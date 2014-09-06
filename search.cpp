#include "search.h"

#include <utility>
#include <memory>
#include <atomic>
#include <chrono>

#include <cstdint>

#include "util.h"
#include "eval/eval.h"
#include "see.h"

namespace
{
	// a score of MATE_MOVING_SIDE means the opponent (of the moving side) is mated on the board
	const static Score MATE_MOVING_SIDE = 30000;

	// a score of MATE_OPPONENT_SIDE means the moving side is mated on the board
	const static Score MATE_OPPONENT_SIDE = -30000;

	const static Score MATE_MOVING_SIDE_THRESHOLD = 20000;
	const static Score MATE_OPPONENT_SIDE_THRESHOLD = -20000;

	// when these mating scores are propagated up, they are adjusted by distance to mate
	inline void AdjustIfMateScore(Score &score)
	{
		if (score > MATE_MOVING_SIDE_THRESHOLD)
		{
			--score;
		}
		else if (score < MATE_OPPONENT_SIDE_THRESHOLD)
		{
			++score;
		}
	}
}

namespace Search
{

static const Depth ID_MAX_DEPTH = 200;

AsyncSearch::AsyncSearch(RootSearchContext &context)
	: m_context(context), m_done(false)
{
}

void AsyncSearch::Start()
{
	std::thread searchThread(&AsyncSearch::RootSearch_, this);

	m_thread = std::move(searchThread);
}

void AsyncSearch::RootSearch_()
{
	double startTime = CurrentTime();

	// this is the time we want to stop searching
	double normaleEndTime = startTime + m_context.timeAlloc.normalTime;

	// this is the time we HAVE to stop searching
	// only the ID controller can make the decision to switch to this end time
	//double absoluteEndTime = startTime + m_context.timeAlloc.maxTime;

	double endTime = normaleEndTime;

	SearchResult latestResult;

	if (m_context.searchType != SearchType_infinite)
	{
		std::thread searchTimerThread(&AsyncSearch::SearchTimer_, this, endTime - CurrentTime());
		m_searchTimerThread = std::move(searchTimerThread);
	}

	for (Depth depth = 1;
			(depth < ID_MAX_DEPTH) &&
			((CurrentTime() < endTime) || (m_context.searchType == SearchType_infinite)) &&
			!m_context.stopRequest;
		 ++depth)
	{
		latestResult.score = Search_(m_context, latestResult.bestMove, m_context.startBoard, SCORE_MIN, SCORE_MAX, depth, 0);

		if (!m_context.stopRequest)
		{
			m_rootResult = latestResult;

			ThinkingOutput thinkingOutput;
			thinkingOutput.nodeCount = m_context.nodeCount;
			thinkingOutput.ply = depth;
			thinkingOutput.pv = m_context.startBoard.MoveToAlg(latestResult.bestMove);
			thinkingOutput.score = latestResult.score;
			thinkingOutput.time = CurrentTime() - startTime;

			m_context.thinkingOutputFunc(thinkingOutput);
		}
	}

	if (m_searchTimerThread.joinable())
	{
		// this thread is only started if we are in time limited search
		m_searchTimerThread.join();
	}

	if (m_context.searchType == SearchType_makeMove)
	{
		std::string bestMove = m_context.startBoard.MoveToAlg(m_rootResult.bestMove);
		m_context.finalMoveFunc(bestMove);
	}

	m_done = true;
}

void AsyncSearch::SearchTimer_(double time)
{
	// this check is required because of GCC (libstdc++) bug #58038: http://gcc.gnu.org/bugzilla/show_bug.cgi?id=58038
	if (time >= 0.0)
	{
		std::this_thread::sleep_for(std::chrono::microseconds(static_cast<uint64_t>(time * 1000000)));
	}

	m_context.stopRequest = true;
}

Score AsyncSearch::Search_(RootSearchContext &context, Move &bestMove, Board &board, Score alpha, Score beta, Depth depth, int32_t ply)
{
	++context.nodeCount;

	if (context.stopRequest)
	{
		// if global stop request is set, we just return any value since it won't be used anyways
		return 0;
	}

	bool isQS = (depth <= 0) && (!board.InCheck());
	bool legalMoveFound = false;

	Score staticEval = Eval::Evaluate(board, alpha, beta);

	if (isQS && staticEval > beta)
	{
		return staticEval;
	}

	MoveList moves;

	if (isQS)
	{
		board.GenerateAllMoves<Board::VIOLENT>(moves);
	}
	else
	{
		board.GenerateAllMoves<Board::ALL>(moves);
	}

	for (size_t i = 0; i < moves.GetSize(); ++i)
	{
		if (isQS && board.IsSeeEligible(moves[i]))
		{
			Score see = StaticExchangeEvaluation(board, moves[i]);

			if (see < 0)
			{
				continue;
			}
		}

		if (board.ApplyMove(moves[i]))
		{
			legalMoveFound = true;

			Score score = -Search_(context, board, -beta, -alpha, depth - 1, ply + 1);

			board.UndoMove();

			if (context.stopRequest)
			{
				return 0;
			}

			AdjustIfMateScore(score);

			if (score > alpha)
			{
				alpha = score;
				bestMove = moves[i];
			}

			if (score >= beta)
			{
				return score;
			}
		}
	}

	if (legalMoveFound)
	{
		if (!context.stopRequest)
		{
			// store TT
		}

		return alpha;
	}
	else
	{
		if (isQS)
		{
			// if we are in qsearch, and there is no legal move, we are at a quiet position, and can return eval
			return staticEval;
		}
		else
		{
			// if we are not in qsearch, and there is no legal move, the game has ended
			if (board.InCheck())
			{
				// if we are in check and have no move to make, we are mated
				return MATE_OPPONENT_SIDE;
			}
			else
			{
				// otherwise this is a stalemate
				return 0;
			}
		}
	}
}

Score AsyncSearch::Search_(RootSearchContext &context, Board &board, Score alpha, Score beta, Depth depth, int32_t ply)
{
	Move dummy;
	return Search_(context, dummy, board, alpha, beta, depth, ply);
}

}
