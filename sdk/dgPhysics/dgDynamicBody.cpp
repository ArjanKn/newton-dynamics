/* Copyright (c) <2003-2016> <Julio Jerez, Newton Game Dynamics>
* 
* This software is provided 'as-is', without any express or implied
* warranty. In no event will the authors be held liable for any damages
* arising from the use of this software.
* 
* Permission is granted to anyone to use this software for any purpose,
* including commercial applications, and to alter it and redistribute it
* freely, subject to the following restrictions:
* 
* 1. The origin of this software must not be misrepresented; you must not
* claim that you wrote the original software. If you use this software
* in a product, an acknowledgment in the product documentation would be
* appreciated but is not required.
* 
* 2. Altered source versions must be plainly marked as such, and must not be
* misrepresented as being the original software.
* 
* 3. This notice may not be removed or altered from any source distribution.
*/

#include "dgPhysicsStdafx.h"
#include "dgWorld.h"
#include "dgDynamicBody.h"
#include "dgCollisionInstance.h"
#include "dgCollisionLumpedMassParticles.h"

//////////////////////////////////////////////////////////////////////
// Construction/Destruction
//////////////////////////////////////////////////////////////////////

dgVector dgDynamicBody::m_equilibriumError2 (DG_ERR_TOLERANCE2);

dgDynamicBody::dgDynamicBody()
	:dgBody()
	,m_externalForce(dgFloat32 (0.0f))
	,m_externalTorque(dgFloat32 (0.0f))
	,m_savedExternalForce(dgFloat32 (0.0f))
	,m_savedExternalTorque(dgFloat32 (0.0f))
	,m_dampCoef(dgFloat32 (0.0f))
	,m_cachedDampCoef(dgFloat32(0.0f))
	,m_cachedTimeStep(dgFloat32(0.0f))
	,m_sleepingCounter(0)
	,m_isInDestructionArrayLRU(0)
	,m_skeleton(NULL)
	,m_applyExtForces(NULL)
	,m_linearDampOn(true)
	,m_angularDampOn(true)
{
	m_type = m_dynamicBody;
	m_rtti |= m_dynamicBodyRTTI;
	dgAssert ( dgInt32 (sizeof (dgDynamicBody) & 0x0f) == 0);
}

dgDynamicBody::dgDynamicBody (dgWorld* const world, const dgTree<const dgCollision*, dgInt32>* const collisionCashe, dgDeserialize serializeCallback, void* const userData, dgInt32 revisionNumber)
	:dgBody(world, collisionCashe, serializeCallback, userData, revisionNumber)
	,m_externalForce(dgFloat32 (0.0f))
	,m_externalTorque(dgFloat32 (0.0f))
	,m_savedExternalForce(dgFloat32 (0.0f))
	,m_savedExternalTorque(dgFloat32 (0.0f))
	,m_dampCoef(dgFloat32 (0.0f))
	,m_cachedDampCoef(dgFloat32(0.0f))
	,m_cachedTimeStep(dgFloat32(0.0f))
	,m_sleepingCounter(0)
	,m_isInDestructionArrayLRU(0)
	,m_skeleton(NULL)
	,m_applyExtForces(NULL)
	,m_linearDampOn(true)
	,m_angularDampOn(true)
{
	dgInt32 val;
	m_type = m_dynamicBody;
	m_rtti |= m_dynamicBodyRTTI;

	m_invWorldInertiaMatrix[3][3] = dgFloat32 (1.0f);
	serializeCallback (userData, &m_mass, sizeof (m_mass));
	serializeCallback (userData, &m_invMass, sizeof (m_invMass));
	serializeCallback (userData, &m_dampCoef, sizeof (m_dampCoef));
	serializeCallback(userData, &val, sizeof (dgInt32));
	m_linearDampOn = (val & 1) ? true : false;
	m_angularDampOn = (val & 2) ? true : false;

}

dgDynamicBody::~dgDynamicBody()
{
	if (m_skeleton) {
		dgSkeletonContainer* const skel = m_skeleton;
		m_skeleton = NULL;
		m_world->DestroySkeletonContainer(skel);
	}
}

void dgDynamicBody::Serialize (const dgTree<dgInt32, const dgCollision*>& collisionRemapId, dgSerialize serializeCallback, void* const userData)
{
	dgBody::Serialize (collisionRemapId, serializeCallback, userData);

	dgInt32 val = (m_linearDampOn ? 1 : 0) & (m_angularDampOn ? 2 : 0) ;
	serializeCallback (userData, &m_mass, sizeof (m_mass));
	serializeCallback (userData, &m_invMass, sizeof (m_invMass));
	serializeCallback (userData, &m_dampCoef, sizeof (m_dampCoef));
	serializeCallback (userData, &val, sizeof (dgInt32));
}


void dgDynamicBody::SetMatrixResetSleep(const dgMatrix& matrix)
{
	dgBody::SetMatrixResetSleep(matrix);
	m_savedExternalForce = dgVector (dgFloat32 (0.0f));
	m_savedExternalTorque = dgVector (dgFloat32 (0.0f));
	CalcInvInertiaMatrix();
}

void dgDynamicBody::SetMatrixNoSleep(const dgMatrix& matrix)
{
	dgBody::SetMatrixNoSleep(matrix);
	CalcInvInertiaMatrix();
}


void dgDynamicBody::AttachCollision (dgCollisionInstance* const collision)
{
	dgBody::AttachCollision(collision);
	if (m_collision->IsType(dgCollision::dgCollisionMesh_RTTI) || m_collision->IsType(dgCollision::dgCollisionScene_RTTI)) {
		//SetMassMatrix (m_mass.m_w, m_mass.m_x, m_mass.m_y, m_mass.m_z);
		SetMassMatrix (m_mass.m_w, CalculateLocalInertiaMatrix());
	}
}


dgVector dgDynamicBody::GetAlpha() const
{
	return m_alpha;
}

dgVector dgDynamicBody::GetAccel() const
{
	return m_accel;
}

void dgDynamicBody::SetAlpha(const dgVector& alpha)
{
	m_alpha = alpha;
}

void dgDynamicBody::SetAccel(const dgVector& accel)
{
	m_accel = accel;
}


bool dgDynamicBody::IsInEquilibrium() const
{
	if (m_equilibrium) {
		dgVector deltaAccel((m_externalForce - m_savedExternalForce).Scale4(m_invMass.m_w));
		dgAssert(deltaAccel.m_w == 0.0f);
		dgFloat32 deltaAccel2 = deltaAccel.DotProduct4(deltaAccel).GetScalar();
		if (deltaAccel2 > DG_ERR_TOLERANCE2) {
			return false;
		}
		dgVector deltaAlpha(m_matrix.UnrotateVector(m_externalTorque - m_savedExternalTorque) * m_invMass);
		dgAssert(deltaAlpha.m_w == 0.0f);
		dgFloat32 deltaAlpha2 = deltaAlpha.DotProduct4(deltaAlpha).GetScalar();
		if (deltaAlpha2 > DG_ERR_TOLERANCE2) {
			return false;
		}
		return true;
	}
	return false;
}

void dgDynamicBody::ApplyExtenalForces (dgFloat32 timestep, dgInt32 threadIndex)
{
	m_externalForce = dgVector::m_zero;
	m_externalTorque = dgVector::m_zero;
	if (m_applyExtForces) {
		m_applyExtForces(*this, timestep, threadIndex);
	}
	m_externalForce += m_impulseForce;
	m_externalTorque += m_impulseTorque;
	m_impulseForce = dgVector::m_zero;
	m_impulseTorque = dgVector::m_zero;
}

void dgDynamicBody::AddDampingAcceleration(dgFloat32 timestep)
{
/*
	if (dgAbs(m_cachedTimeStep - timestep) > dgFloat32(1.0e-6f)) {
		m_cachedTimeStep = timestep;
		const dgFloat32 tau = dgFloat32(1.0f) / (dgFloat32(60.0f) * timestep);
		m_cachedDampCoef.m_x = dgPow(dgFloat32(1.0f) - m_dampCoef.m_x, tau);
		m_cachedDampCoef.m_y = dgPow(dgFloat32(1.0f) - m_dampCoef.m_y, tau);
		m_cachedDampCoef.m_z = dgPow(dgFloat32(1.0f) - m_dampCoef.m_z, tau);
		m_cachedDampCoef.m_w = dgPow(dgFloat32(1.0f) - m_dampCoef.m_w, tau);
	} 
*/
	dgVector damp (GetDampCoeffcient (timestep));
	if (m_linearDampOn) {
		m_veloc = m_veloc.Scale4(damp.m_w);
	}

	if (m_angularDampOn) {
		dgVector omegaDamp(damp & dgVector::m_triplexMask);
		dgVector omega(m_matrix.UnrotateVector(m_omega) * omegaDamp);
		m_omega = m_matrix.RotateVector(omega);
	}
}


void dgDynamicBody::InvalidateCache ()
{
	m_sleepingCounter = 0;
	m_savedExternalForce = dgVector (dgFloat32 (0.0f));
	m_savedExternalTorque = dgVector (dgFloat32 (0.0f));
	dgBody::InvalidateCache ();
}

void dgDynamicBody::IntegrateOpenLoopExternalForce(dgFloat32 timestep)
{
	if (!m_equilibrium) {
		if (!m_collision->IsType(dgCollision::dgCollisionLumpedMass_RTTI)) {
			dgVector localOmega (m_matrix.UnrotateVector(m_omega));
			dgVector localExternalTorque (m_matrix.UnrotateVector(m_externalTorque));
			dgVector localAlphaStep ((localExternalTorque - localOmega.CrossProduct3(localOmega * m_mass)) * m_invMass);

			const dgFloat32 localAlphaErr = dgFloat32 (0.01f);
			if (localAlphaStep.DotProduct4(localAlphaStep).GetScalar() < (localAlphaErr * localAlphaErr)) {
				// omega step is very small no point on doing implicit integration 
				localOmega += localAlphaStep.Scale4(timestep);
			} else {
				// Simple forward Euler in local space step no enough to cope with skew and high angular velocities
				// Using simple backward Euler in local space step, implicit integration in local space. 
				// use angular velocity at dt, to solve equation
				// f(w + dw) = f(w0) + f'(w + dw) * dw
				// let dw = w * dt
				// and calculating dw as the  dw = f(w) | dwx, dwy, dwz

				// dw/dt = (Tl - (wl x (wl * Il)) * Il^1
				// expanding f(w) 
				// f(wx) = (Tx - (Iz - Iy) * wy * wz) / Ix 
				// f(wy) = (Ty - (Ix - Iz) * wz * wx) / Iy
				// f(wz) = (Tz - (Iy - Ix) * wx * wy) / Iz

				/*
				// using Mathematica script to calculate the derivatives of the Taylor expression
				CreateDocument[{TextCell["Wx ="], Wxx,
				TextCell["dwx/dwx ="], D[Wxx, { wx, 1 }],
				TextCell["dwx/dwy ="], D[Wxx, { wy, 1 }],
				TextCell["dwx/dwz ="], D[Wxx, { wz, 1 }]}]

				CreateDocument[{TextCell["Wy ="], Wyy,
				TextCell["dwy/dwx ="], D[Wyy, { wx, 1 }],
				TextCell["dwy/dwy ="], D[Wyy, { wy, 1 }],
				TextCell["dwy/dwz ="], D[Wyy, { wz, 1 }]}]

				CreateDocument[{TextCell["Wz ="], Wzz,
				TextCell["dwz/dwx ="], D[Wzz, { wx, 1 }],
				TextCell["dwz/dwy ="], D[Wzz, { wy, 1 }],
				TextCell["dwz/dwz ="], D[Wzz, { wz, 1 }]}]
				*/

				dgFloat32 dt = dgFloat32 (0.5f) * timestep;
#if 0
				//two step of implicit integration 
				for (dgInt32 i = 0; i < 2; i ++) 
				{
					// calculate gradient
					dgVector deltaToque (localExternalTorque - localOmega.CrossProduct3(localOmega * m_mass));
					dgVector gradientStep(deltaToque.Scale4(dt));
#else
				//Note: two step of implicit integration loses too much energy, it is better to use a semi implicit
				// that is, calculate derivative at half the time step instead of the end, similar to mid point Euler
				{
					// calculate gradient
					dgVector deltaToque(localExternalTorque - localOmega.CrossProduct3(localOmega * m_mass));
					dgVector gradientStep(deltaToque.Scale4(timestep));
#endif
					dgVector dw(localOmega.Scale4(dt));
					dgFloat32 jacobianMatrix[3][3];
					// calculates Jacobian matrix
					//dWx / dwx = Ix
					//dWx / dwy = (Iz - Iy) * wz * dt
					//dWx / dwz = (Iz - Iy) * wy * dt
					jacobianMatrix[0][0] = m_mass[0];
					jacobianMatrix[0][1] = (m_mass[2] - m_mass[1]) * dw[2];
					jacobianMatrix[0][2] = (m_mass[2] - m_mass[1]) * dw[1];

					//dWy / dwx = (Ix - Iz) * wz * dt
					//dWy / dwy = Iy				 
					//dWy / dwz = (Ix - Iz) * wx * dt
					jacobianMatrix[1][0] = (m_mass[0] - m_mass[2]) * dw[2];
					jacobianMatrix[1][1] = m_mass[1];
					jacobianMatrix[1][2] = (m_mass[0] - m_mass[2]) * dw[0];

					//dWz / dwx = (Iy - Ix) * wy * dt 
					//dWz / dwy = (Iy - Ix) * wx * dt 
					//dWz / dwz = Iz
					jacobianMatrix[2][0] = (m_mass[1] - m_mass[0]) * dw[1];
					jacobianMatrix[2][1] = (m_mass[1] - m_mass[0]) * dw[0];
					jacobianMatrix[2][2] = m_mass[2];

					dgSolveGaussian(3, &jacobianMatrix[0][0], &gradientStep[0]);
					localOmega += gradientStep;
				}
			}

			m_accel = m_externalForce.Scale4(m_invMass.m_w);
			m_alpha = m_matrix.RotateVector(localAlphaStep);

			m_veloc += m_accel.Scale4(timestep);
			m_omega = m_matrix.RotateVector(localOmega);
		} else {
			dgCollisionLumpedMassParticles* const lumpedMassShape = (dgCollisionLumpedMassParticles*)m_collision->m_childShape;
			lumpedMassShape->IntegrateForces(timestep);
		}
	} else {
		m_accel = dgVector::m_zero;
		m_alpha = dgVector::m_zero;
	}
}


dgDynamicBodyAsymetric::dgDynamicBodyAsymetric()
	:dgDynamicBody()
	,m_principalAxis(dgGetIdentityMatrix())
{
	m_type = m_dynamicBody;
	m_rtti |= m_dynamicBodyAsymentricRTTI;
	dgAssert(dgInt32(sizeof(dgDynamicBody) & 0x0f) == 0);

}

dgDynamicBodyAsymetric::dgDynamicBodyAsymetric(dgWorld* const world, const dgTree<const dgCollision*, dgInt32>* const collisionNode, dgDeserialize serializeCallback, void* const userData, dgInt32 revisionNumber)
	:dgDynamicBody(world, collisionNode, serializeCallback, userData, revisionNumber)
	,m_principalAxis(dgGetIdentityMatrix())
{
	m_type = m_dynamicBody;
	m_rtti |= m_dynamicBodyRTTI;
	serializeCallback(userData, &m_principalAxis, sizeof(m_principalAxis));
}

void dgDynamicBodyAsymetric::Serialize(const dgTree<dgInt32, const dgCollision*>& collisionRemapId, dgSerialize serializeCallback, void* const userData)
{
	dgDynamicBody::Serialize(collisionRemapId, serializeCallback, userData);
	serializeCallback(userData, &m_principalAxis, sizeof(m_principalAxis));
}


void dgDynamicBodyAsymetric::SetMassMatrix(dgFloat32 mass, const dgMatrix& inertia)
{
	dgVector II;
	m_principalAxis = inertia;
	m_principalAxis.EigenVectors(II);
	dgMatrix massMatrix(dgGetIdentityMatrix());
	massMatrix[0][0] = II[0];
	massMatrix[1][1] = II[1];
	massMatrix[2][2] = II[2];
	dgBody::SetMassMatrix(mass, massMatrix);
}

dgMatrix dgDynamicBodyAsymetric::CalculateLocalInertiaMatrix() const
{
	dgMatrix matrix(m_principalAxis);
	matrix.m_posit = dgVector::m_wOne;
	dgMatrix diagonal(dgGetIdentityMatrix());
	diagonal[0][0] = m_mass[0];
	diagonal[1][1] = m_mass[1];
	diagonal[2][2] = m_mass[2];
	return matrix * diagonal * matrix.Inverse();
}

dgMatrix dgDynamicBodyAsymetric::CalculateInertiaMatrix() const
{
	dgMatrix matrix(m_principalAxis * m_matrix);
	matrix.m_posit = dgVector::m_wOne;
	dgMatrix diagonal(dgGetIdentityMatrix());
	diagonal[0][0] = m_mass[0];
	diagonal[1][1] = m_mass[1];
	diagonal[2][2] = m_mass[2];
	return matrix * diagonal * matrix.Inverse();
}

dgMatrix dgDynamicBodyAsymetric::CalculateInvInertiaMatrix() const
{
	dgMatrix matrix(m_principalAxis * m_matrix);
	matrix.m_posit = dgVector::m_wOne;
	dgMatrix diagonal(dgGetIdentityMatrix());
	diagonal[0][0] = m_invMass[0];
	diagonal[1][1] = m_invMass[1];
	diagonal[2][2] = m_invMass[2];
	return matrix * diagonal * matrix.Inverse();
}

void dgDynamicBodyAsymetric::IntegrateOpenLoopExternalForce(dgFloat32 timestep)
{
//	dgMatrix saveMatrix(m_matrix);
//  m_matrix = m_principalAxis * m_matrix;
	dgDynamicBody::IntegrateOpenLoopExternalForce(timestep);
//	m_matrix = saveMatrix;
}
