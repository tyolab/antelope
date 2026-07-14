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
long ANT_pq_codec::train(const float *vectors, long long dimension, long long m, long long k, long long n, float *codebook)
{
long long sub, s, c, d, i;

if (m < 1 || dimension % m != 0)
	return 1;

sub = dimension / m;

if (n == 0)
	{
	memset(codebook, 0, (size_t)(m * k * sub) * sizeof(float));
	return 0;
	}

std::vector<long> assignment(n);
std::vector<long long> counts(k);
std::vector<double> sums(k * sub);

for (s = 0; s < m; s++)
	{
	float *centroids = codebook + s * k * sub;		// this subspace's k centroids
	const float *subvecs_base = vectors + s * sub;		// offset into each row

	/* Init: first-k-distinct sub-vectors (bytewise compare); if fewer than k
	   distinct exist, repeat the last distinct one for the remainder. */
	long long distinct_found = 0;
	for (i = 0; i < n && distinct_found < k; i++)
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
		memset(centroids, 0, (size_t)(k * sub) * sizeof(float));
		distinct_found = 1;
		}
	for (c = distinct_found; c < k; c++)
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
			for (c = 1; c < k; c++)
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
		for (c = 0; c < k; c++)
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
		for (c = 0; c < k; c++)
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
void ANT_pq_codec::encode(const float *vector, long long dimension, long long m, long long k, const float *codebook, unsigned char *codes)
{
long long sub, s, c, d;

sub = dimension / m;

for (s = 0; s < m; s++)
	{
	const float *v = vector + s * sub;
	const float *centroids = codebook + s * k * sub;
	long best = 0;
	double best_dist = 0;
	for (d = 0; d < sub; d++)
		{
		double diff = (double)v[d] - (double)centroids[d];
		best_dist += diff * diff;
		}
	for (c = 1; c < k; c++)
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
void ANT_pq_codec::adc_table(const float *query, long long dimension, long long m, long long k, const float *codebook, long metric, double *table)
{
long long sub, s, c, d;

sub = dimension / m;

for (s = 0; s < m; s++)
	{
	const float *q = query + s * sub;
	const float *centroids = codebook + s * k * sub;
	for (c = 0; c < k; c++)
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
		table[s * k + c] = value;
		}
	}
}

/*
	ANT_pq_codec::adc_score()
	---------------------------
*/
double ANT_pq_codec::adc_score(const unsigned char *codes, long long m, long long k, const double *table)
{
long long s;
double total = 0;

for (s = 0; s < m; s++)
	total += table[s * k + codes[s]];

return total;
}

/*
	ANT_pq_codec::reconstruct()
	-----------------------------
*/
void ANT_pq_codec::reconstruct(const unsigned char *codes, long long dimension, long long m, long long k, const float *codebook, float *out)
{
long long sub, s;

sub = dimension / m;

for (s = 0; s < m; s++)
	{
	const float *centroids = codebook + s * k * sub;
	const float *cent = centroids + codes[s] * sub;
	memcpy(out + s * sub, cent, (size_t)sub * sizeof(float));
	}
}

/*
	ANT_pq_codec::bits_for_k()
	----------------------------
	log2(k) for a power of two in [2,256]; -1 otherwise.
*/
long ANT_pq_codec::bits_for_k(long long k)
{
if (k < 2 || k > 256)
	return -1;
long bits = 0;
long long v = k;
while ((v & 1) == 0) { v >>= 1; bits++; }
return (v == 1) ? bits : -1;			// v==1 iff k was a power of two
}

/*
	ANT_pq_codec::pack_codes()
	----------------------------
	Pack m byte-codes (each < 2^bits) LSB-first into (m*bits+7)/8 bytes:
	code s occupies bit positions [s*bits, (s+1)*bits); trailing bits zero.
	Deterministic. bits==8 is a straight memcpy (identity).
*/
void ANT_pq_codec::pack_codes(const unsigned char *codes, long long m, long long bits, unsigned char *packed)
{
long long nbytes = (m * bits + 7) / 8;
if (bits == 8)
	{ memcpy(packed, codes, (size_t)m); return; }
memset(packed, 0, (size_t)(nbytes > 0 ? nbytes : 1));
for (long long s = 0; s < m; s++)
	{
	unsigned long v = codes[s];
	long long base = s * bits;
	for (long long j = 0; j < bits; j++)
		if (v & (1UL << j))
			packed[(base + j) >> 3] |= (unsigned char)(1u << ((base + j) & 7));
	}
}

/*
	ANT_pq_codec::unpack_codes()
	------------------------------
	Inverse of pack_codes: writes m byte-codes.
*/
void ANT_pq_codec::unpack_codes(const unsigned char *packed, long long m, long long bits, unsigned char *codes)
{
if (bits == 8)
	{ memcpy(codes, packed, (size_t)m); return; }
for (long long s = 0; s < m; s++)
	{
	unsigned long v = 0;
	long long base = s * bits;
	for (long long j = 0; j < bits; j++)
		if (packed[(base + j) >> 3] & (1u << ((base + j) & 7)))
			v |= (1UL << j);
	codes[s] = (unsigned char)v;
	}
}

/*
	Deterministic cyclic symmetric Jacobi eigensolver. `a` (n*n, row-major,
	symmetric) is DESTROYED; eigenvalues -> eigval[n]; eigenvectors -> columns
	of V[n*n]. Fixed sweep order + fixed cap => deterministic on one platform.
*/
static void ant_pq_jacobi(double *a, long long n, double *eigval, double *V)
{
long long i, j, p, q, k, sweep;
for (i = 0; i < n; i++)
	for (j = 0; j < n; j++)
		V[i*n + j] = (i == j) ? 1.0 : 0.0;
for (sweep = 0; sweep < 100; sweep++)
	{
	double off = 0.0;
	for (p = 0; p < n; p++)
		for (q = p+1; q < n; q++)
			off += a[p*n + q] * a[p*n + q];
	if (off <= 1e-30)
		break;
	for (p = 0; p < n; p++)
		for (q = p+1; q < n; q++)
			{
			double apq = a[p*n + q];
			if (fabs(apq) <= 1e-300)
				continue;
			double app = a[p*n + p], aqq = a[q*n + q];
			double phi = 0.5 * (aqq - app) / apq;
			double t = (phi >= 0 ? 1.0 : -1.0) / (fabs(phi) + sqrt(phi*phi + 1.0));
			double c = 1.0 / sqrt(t*t + 1.0);
			double s = t * c;
			for (k = 0; k < n; k++)
				{
				double akp = a[k*n + p], akq = a[k*n + q];
				a[k*n + p] = c*akp - s*akq;
				a[k*n + q] = s*akp + c*akq;
				}
			for (k = 0; k < n; k++)
				{
				double apk = a[p*n + k], aqk = a[q*n + k];
				a[p*n + k] = c*apk - s*aqk;
				a[q*n + k] = s*apk + c*aqk;
				}
			for (k = 0; k < n; k++)
				{
				double vkp = V[k*n + p], vkq = V[k*n + q];
				V[k*n + p] = c*vkp - s*vkq;
				V[k*n + q] = s*vkp + c*vkq;
				}
			}
	}
for (i = 0; i < n; i++)
	eigval[i] = a[i*n + i];
}

long ANT_pq_codec::train_rotation(const float *vectors, long long dimension, long long m, long long n, float *R)
{
long long i, j, d, D = dimension;
if (m < 1 || dimension % m != 0 || n <= 0)
	return 1;

// second-moment matrix M = sum_i x_i x_i^T  (D*D, symmetric, NO centering -> metric-preserving)
std::vector<double> M((size_t)(D*D), 0.0);
for (i = 0; i < n; i++)
	{
	const float *x = vectors + i * D;
	for (d = 0; d < D; d++)
		{
		double xd = (double)x[d];
		double *row = &M[(size_t)(d*D)];
		for (j = 0; j < D; j++)
			row[j] += xd * (double)x[j];
		}
	}

std::vector<double> eigval((size_t)D), V((size_t)(D*D));
ant_pq_jacobi(&M[0], D, &eigval[0], &V[0]);   // V columns = eigenvectors (M destroyed)

// sign-canonicalize each eigenvector column: force largest-|component| positive (kills sign ambiguity)
for (j = 0; j < D; j++)
	{
	long long best = 0; double bestmag = -1.0;
	for (d = 0; d < D; d++)
		{ double mg = fabs(V[(size_t)(d*D + j)]); if (mg > bestmag) { bestmag = mg; best = d; } }
	if (V[(size_t)(best*D + j)] < 0.0)
		for (d = 0; d < D; d++)
			V[(size_t)(d*D + j)] = -V[(size_t)(d*D + j)];
	}

// eigenvalue-balanced subspace allocation: assign each eigenvector (desc eigenvalue) to the
// not-yet-full subspace with the smallest running log-variance product -> balanced subspaces.
long long sub = D / m;
std::vector<long long> idx((size_t)D);
for (d = 0; d < D; d++) idx[d] = d;
// stable sort by descending eigenvalue (deterministic tie-break by index via <= comparison)
for (i = 0; i < D; i++)
	for (j = i+1; j < D; j++)
		if (eigval[idx[j]] > eigval[idx[i]])
			{ long long tmp = idx[i]; idx[i] = idx[j]; idx[j] = tmp; }
std::vector<double> bucket_log((size_t)m, 0.0);
std::vector<long long> bucket_cnt((size_t)m, 0);
std::vector<long long> order((size_t)D, -1);   // output row -> eigenvector index
std::vector<long long> bucket_fill((size_t)m, 0);
for (i = 0; i < D; i++)
	{
	long long e = idx[i];
	long long chosen = -1; double best_log = 0;
	for (long long b = 0; b < m; b++)
		{
		if (bucket_cnt[b] >= sub) continue;
		if (chosen < 0 || bucket_log[b] < best_log) { chosen = b; best_log = bucket_log[b]; }
		}
	double lam = eigval[e]; if (lam < 1e-12) lam = 1e-12;
	bucket_log[chosen] += log(lam);
	// place at subspace `chosen`'s next contiguous output row
	order[(size_t)(chosen*sub + bucket_fill[chosen])] = e;
	bucket_fill[chosen]++;
	bucket_cnt[chosen]++;
	}

// R rows = allocated eigenvectors (row r takes eigenvector `order[r]`, i.e. column order[r] of V)
for (i = 0; i < D; i++)
	{
	long long e = order[i];
	for (d = 0; d < D; d++)
		R[(size_t)(i*D + d)] = (float)V[(size_t)(d*D + e)];
	}
return 0;
}

void ANT_pq_codec::apply_rotation(const float *vec, long long dimension, const float *R, float *out)
{
long long r, d, D = dimension;
for (r = 0; r < D; r++)
	{
	double acc = 0.0;
	const float *Rr = R + r*D;
	for (d = 0; d < D; d++)
		acc += (double)Rr[d] * (double)vec[d];
	out[r] = (float)acc;
	}
}

void ANT_pq_codec::apply_rotation_transpose(const float *vec, long long dimension, const float *R, float *out)
{
long long r, d, D = dimension;
for (d = 0; d < D; d++)
	{
	double acc = 0.0;
	for (r = 0; r < D; r++)
		acc += (double)R[r*D + d] * (double)vec[r];
	out[d] = (float)acc;
	}
}
