/*
	PQ_CODEC.CPP -- see pq_codec.h for the contract. Deterministic k-means
	(first-K-distinct init, Lloyd iterations, ties broken by lowest index,
	empty clusters keep their previous centroid) so that a rebuild from the
	same input vectors is byte-identical.
*/
#include "pq_codec.h"
#include <math.h>
#include <string.h>
#include <vector>

/*
	ANT_pq_codec::train()
	----------------------
*/
long ANT_pq_codec::train(const float *vectors, long long dimension, long long m, long long n, float *codebook)
{
long long sub, s, c, d, i;

if (m < 1 || dimension % m != 0)
	return 1;

sub = dimension / m;

if (n == 0)
	{
	memset(codebook, 0, (size_t)(m * K * sub) * sizeof(float));
	return 0;
	}

std::vector<long> assignment(n);
std::vector<long long> counts(K);
std::vector<double> sums(K * sub);

for (s = 0; s < m; s++)
	{
	float *centroids = codebook + s * K * sub;		// this subspace's K centroids
	const float *subvecs_base = vectors + s * sub;		// offset into each row

	/* Init: first-K-distinct sub-vectors (bytewise compare); if fewer than K
	   distinct exist, repeat the last distinct one for the remainder. */
	long long distinct_found = 0;
	for (i = 0; i < n && distinct_found < K; i++)
		{
		const float *cand = subvecs_base + i * dimension;
		int is_distinct = 1;
		for (c = 0; c < distinct_found; c++)
			if (memcmp(centroids + c * sub, cand, (size_t)sub * sizeof(float)) == 0)
				{
				is_distinct = 0;
				break;
				}
		if (is_distinct)
			{
			memcpy(centroids + distinct_found * sub, cand, (size_t)sub * sizeof(float));
			distinct_found++;
			}
		}
	if (distinct_found == 0)
		{
		/* n > 0 guaranteed here, but guard anyway: zero-fill */
		memset(centroids, 0, (size_t)(K * sub) * sizeof(float));
		distinct_found = 1;
		}
	for (c = distinct_found; c < K; c++)
		memcpy(centroids + c * sub, centroids + (distinct_found - 1) * sub, (size_t)sub * sizeof(float));

	/* Lloyd iterations */
	long long iter;
	for (iter = 0; iter < KMEANS_ITERS; iter++)
		{
		/* assign */
		for (i = 0; i < n; i++)
			{
			const float *v = subvecs_base + i * dimension;
			long best = 0;
			double best_dist = 0;
			for (d = 0; d < sub; d++)
				{
				double diff = (double)v[d] - (double)centroids[d];
				best_dist += diff * diff;
				}
			for (c = 1; c < K; c++)
				{
				const float *cent = centroids + c * sub;
				double dist = 0;
				for (d = 0; d < sub; d++)
					{
					double diff = (double)v[d] - (double)cent[d];
					dist += diff * diff;
					}
				if (dist < best_dist)
					{
					best_dist = dist;
					best = (long)c;
					}
				}
			assignment[i] = best;
			}

		/* recompute */
		for (c = 0; c < K; c++)
			{
			counts[c] = 0;
			for (d = 0; d < sub; d++)
				sums[c * sub + d] = 0.0;
			}
		for (i = 0; i < n; i++)
			{
			long a = assignment[i];
			const float *v = subvecs_base + i * dimension;
			counts[a]++;
			for (d = 0; d < sub; d++)
				sums[(long long)a * sub + d] += (double)v[d];
			}
		for (c = 0; c < K; c++)
			{
			if (counts[c] == 0)
				continue;		// keep previous centroid (deterministic, no drift)
			for (d = 0; d < sub; d++)
				centroids[c * sub + d] = (float)(sums[c * sub + d] / (double)counts[c]);
			}
		}
	}

return 0;
}

/*
	ANT_pq_codec::encode()
	-----------------------
*/
void ANT_pq_codec::encode(const float *vector, long long dimension, long long m, const float *codebook, unsigned char *codes)
{
long long sub, s, c, d;

sub = dimension / m;

for (s = 0; s < m; s++)
	{
	const float *v = vector + s * sub;
	const float *centroids = codebook + s * K * sub;
	long best = 0;
	double best_dist = 0;
	for (d = 0; d < sub; d++)
		{
		double diff = (double)v[d] - (double)centroids[d];
		best_dist += diff * diff;
		}
	for (c = 1; c < K; c++)
		{
		const float *cent = centroids + c * sub;
		double dist = 0;
		for (d = 0; d < sub; d++)
			{
			double diff = (double)v[d] - (double)cent[d];
			dist += diff * diff;
			}
		if (dist < best_dist)
			{
			best_dist = dist;
			best = (long)c;
			}
		}
	codes[s] = (unsigned char)best;
	}
}

/*
	ANT_pq_codec::adc_table()
	--------------------------
*/
void ANT_pq_codec::adc_table(const float *query, long long dimension, long long m, const float *codebook, long metric, double *table)
{
long long sub, s, c, d;

sub = dimension / m;

for (s = 0; s < m; s++)
	{
	const float *q = query + s * sub;
	const float *centroids = codebook + s * K * sub;
	for (c = 0; c < K; c++)
		{
		const float *cent = centroids + c * sub;
		double value;
		if (metric == METRIC_L2)
			{
			value = 0;
			for (d = 0; d < sub; d++)
				{
				double diff = (double)q[d] - (double)cent[d];
				value += diff * diff;
				}
			value = -value;
			}
		else		// METRIC_DOT or METRIC_COSINE
			{
			value = 0;
			for (d = 0; d < sub; d++)
				value += (double)q[d] * (double)cent[d];
			}
		table[s * K + c] = value;
		}
	}
}

/*
	ANT_pq_codec::adc_score()
	---------------------------
*/
double ANT_pq_codec::adc_score(const unsigned char *codes, long long m, const double *table)
{
long long s;
double total = 0;

for (s = 0; s < m; s++)
	total += table[s * K + codes[s]];

return total;
}

/*
	ANT_pq_codec::reconstruct()
	-----------------------------
*/
void ANT_pq_codec::reconstruct(const unsigned char *codes, long long dimension, long long m, const float *codebook, float *out)
{
long long sub, s;

sub = dimension / m;

for (s = 0; s < m; s++)
	{
	const float *centroids = codebook + s * K * sub;
	const float *cent = centroids + codes[s] * sub;
	memcpy(out + s * sub, cent, (size_t)sub * sizeof(float));
	}
}
